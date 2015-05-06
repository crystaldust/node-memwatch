/*
 * 2012|lloyd|http://wtfpl.org
 */
#include <iostream>
#include "platformcompat.hh"
#include "memwatch.hh"
#include "heapdiff.hh"
#include "util.hh"

#include <node.h>
#include <node_version.h>

#include <string>
#include <cstring>
#include <sstream>

#include <math.h> // for pow
#include <time.h> // for time
#include <uv.h>

using namespace v8;
using namespace node;

Persistent<Object> g_context;
Persistent<Function> g_cb;

struct Baton {
    uv_work_t req;
    size_t heapUsage;
    GCType type;
    GCCallbackFlags flags;
};

static const unsigned int RECENT_PERIOD = 10;
static const unsigned int ANCIENT_PERIOD = 120;

static struct
{
    // counts of different types of gc events
    unsigned int gc_full;
    unsigned int gc_inc;
    unsigned int gc_compact;

    // last base heap size as measured *right* after GC
    unsigned int last_base;

    // the estimated "base memory" usage of the javascript heap
    // over the RECENT_PERIOD number of GC runs
    unsigned int base_recent;

    // the estimated "base memory" usage of the javascript heap
    // over the ANCIENT_PERIOD number of GC runs
    unsigned int base_ancient;

    // the most extreme values we've seen for base heap size
    unsigned int base_max;
    unsigned int base_min;

    // leak detection!

    // the period from which this leak analysis starts
    time_t leak_time_start;
    // the base memory for the detection period
    time_t leak_base_start;
    // the number of consecutive compactions for which we've grown
    unsigned int consecutive_growth;
} s_stats;

static Handle<Value> getLeakReport(size_t heapUsage)
{
    Isolate *isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    size_t growth = heapUsage - s_stats.leak_base_start;
    int now = time(NULL);
    int delta = now - s_stats.leak_time_start;

    Local<Object> leakReport = Object::New(isolate);
    leakReport->Set(String::NewFromUtf8(isolate,"start"), NODE_UNIXTIME_V8(s_stats.leak_time_start));
    leakReport->Set(String::NewFromUtf8(isolate,"end"), NODE_UNIXTIME_V8(now));
    leakReport->Set(String::NewFromUtf8(isolate,"growth"), Integer::New(isolate, growth));

    std::stringstream ss;
    ss << "heap growth over 5 consecutive GCs ("
       << mw_util::niceDelta(delta) << ") - "
       << mw_util::niceSize(growth / ((double) delta / (60.0 * 60.0))) << "/hr";

    leakReport->Set(String::NewFromUtf8(isolate,"reason"), String::NewFromUtf8(isolate,ss.str().c_str()));

    return scope.Escape( leakReport );
}

static void AsyncMemwatchAfter(uv_work_t* request) {
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    Local<Object> context = Local<Object>::New( isolate, g_context );
    Local<Function> cb = Local<Function>::New( isolate, g_cb );

    Baton * b = (Baton *) request->data;

    // do the math in C++, permanent
    // record the type of GC event that occured
    if (b->type == kGCTypeMarkSweepCompact) s_stats.gc_full++;
    else s_stats.gc_inc++;


    if (
#if NODE_VERSION_AT_LEAST(0,8,0)
        b->type == kGCTypeMarkSweepCompact
#else
        b->flags == kGCCallbackFlagCompacted
#endif
        ) {
        // leak detection code.  has the heap usage grown?
        if (s_stats.last_base < b->heapUsage) {
            if (s_stats.consecutive_growth == 0) {
                s_stats.leak_time_start = time(NULL);
                s_stats.leak_base_start = b->heapUsage;
            }

            s_stats.consecutive_growth++;

            // consecutive growth over 5 GCs suggests a leak
            if (s_stats.consecutive_growth >= 5) {
                // reset to zero
                s_stats.consecutive_growth = 0;

                // emit a leak report!
                Handle<Value> argv[3];
                argv[0] = Boolean::New(isolate, false);
                // the type of event to emit
                argv[1] = String::NewFromUtf8(isolate,"leak");
                argv[2] = getLeakReport(b->heapUsage);
                cb->Call(context, 3, argv);
            }
        } else {
            s_stats.consecutive_growth = 0;
        }

        // update last_base
        s_stats.last_base = b->heapUsage;

        // update compaction count
        s_stats.gc_compact++;

        // the first ten compactions we'll use a different algorithm to
        // dampen out wider memory fluctuation at startup
        if (s_stats.gc_compact < RECENT_PERIOD) {
            double decay = pow(s_stats.gc_compact / RECENT_PERIOD, 2.5);
            decay *= s_stats.gc_compact;
            if (ISINF(decay) || ISNAN(decay)) decay = 0;
            s_stats.base_recent = ((s_stats.base_recent * decay) +
                                   s_stats.last_base) / (decay + 1);

            decay = pow(s_stats.gc_compact / RECENT_PERIOD, 2.4);
            decay *= s_stats.gc_compact;
            s_stats.base_ancient = ((s_stats.base_ancient * decay) +
                                    s_stats.last_base) /  (1 + decay);

        } else {
            s_stats.base_recent = ((s_stats.base_recent * (RECENT_PERIOD - 1)) +
                                   s_stats.last_base) / RECENT_PERIOD;
            double decay = FMIN(ANCIENT_PERIOD, s_stats.gc_compact);
            s_stats.base_ancient = ((s_stats.base_ancient * (decay - 1)) +
                                    s_stats.last_base) / decay;
        }

        // only record min/max after 3 gcs to let initial instability settle
        if (s_stats.gc_compact >= 3) {
            if (!s_stats.base_min || s_stats.base_min > s_stats.last_base) {
                s_stats.base_min = s_stats.last_base;
            }

            if (!s_stats.base_max || s_stats.base_max < s_stats.last_base) {
                s_stats.base_max = s_stats.last_base;
            }
        }


        // if there are any listeners, it's time to emit!
        if (!g_cb.IsEmpty()) {
            Handle<Value> argv[3];

            // magic argument to indicate to the callback all we want to know is whether there are
            // listeners (here we don't)
            argv[0] = Boolean::New(isolate, true);

            Handle<Value> haveListeners = cb->Call( context, 1, argv);

            if (haveListeners->BooleanValue()) {
                double ut= 0.0;
                if (s_stats.base_ancient) {
                    ut = (double) ROUND(((double) (s_stats.base_recent - s_stats.base_ancient) /
                                         (double) s_stats.base_ancient) * 1000.0) / 10.0;
                }

                // ok, there are listeners, we actually must serialize and emit this stats event
                Local<Object> stats = Object::New(isolate);
                stats->Set(String::NewFromUtf8(isolate,"num_full_gc"), Integer::New(isolate, s_stats.gc_full));
                stats->Set(String::NewFromUtf8(isolate,"num_inc_gc"), Integer::New(isolate, s_stats.gc_inc));
                stats->Set(String::NewFromUtf8(isolate,"heap_compactions"), Integer::New(isolate, s_stats.gc_compact));
                stats->Set(String::NewFromUtf8(isolate,"usage_trend"), Number::New(isolate, ut));
                stats->Set(String::NewFromUtf8(isolate,"estimated_base"), Integer::New(isolate, s_stats.base_recent));
                stats->Set(String::NewFromUtf8(isolate,"current_base"), Integer::New(isolate, s_stats.last_base));
                stats->Set(String::NewFromUtf8(isolate,"min"), Integer::New(isolate, s_stats.base_min));
                stats->Set(String::NewFromUtf8(isolate,"max"), Integer::New(isolate, s_stats.base_max));
                argv[0] = Boolean::New(isolate, false);
                // the type of event to emit
                argv[1] = String::NewFromUtf8(isolate,"stats");
                argv[2] = stats;
                cb->Call(context, 3, argv);
            }
        }
    }

    delete b;
}


static void noop_work_func(uv_work_t *) { }

void memwatch::after_gc(GCType type, GCCallbackFlags flags)
{
    if (heapdiff::HeapDiff::InProgress()) return;

    Isolate *isolate = Isolate::GetCurrent();

    Baton * baton = new Baton;
    v8::HeapStatistics hs;

    isolate->GetHeapStatistics(&hs);

    baton->heapUsage = hs.used_heap_size();
    baton->type = type;
    baton->flags = flags;
    baton->req.data = (void *) baton;

    // schedule our work to run in a moment, once gc has fully completed.
    // 
    // here we pass a noop work function to work around a flaw in libuv, 
    // uv_queue_work on unix works fine, but will will crash on
    // windows.  see: https://github.com/joyent/libuv/pull/629  
    uv_queue_work(uv_default_loop(), &(baton->req),
		  noop_work_func, (uv_after_work_cb)AsyncMemwatchAfter);

    //uv_loop_close( loop );
}

void memwatch::upon_gc(const FunctionCallbackInfo<v8::Value>& args) {
    Isolate *isolate = args.GetIsolate();
    if (args.Length() >= 1 && args[0]->IsFunction()) {
        g_cb.Reset( isolate, Handle<Function>::Cast( args[0] ) );
        g_context.Reset( isolate, isolate->GetCallingContext()->Global() );
    }
}

void memwatch::trigger_gc(const FunctionCallbackInfo<v8::Value>& args) {
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);

    while(!isolate->IdleNotification(1000)) {};

    args.GetReturnValue().Set( Undefined( Isolate::GetCurrent() ) );
}
