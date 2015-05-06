/*
 * 2012|lloyd|http://wtfpl.org
 */

#include "heapdiff.hh"
#include "util.hh"

#include <node.h>

#include <map>
#include <string>
#include <set>
#include <vector>

#include <stdlib.h> // abs()
#include <time.h>   // time()

using namespace v8;
using namespace node;
using namespace std;

static bool s_inProgress = false;
static time_t s_startTime;

bool heapdiff::HeapDiff::InProgress() 
{
    return s_inProgress;
}

heapdiff::HeapDiff::HeapDiff() : ObjectWrap(), before(NULL), after(NULL),
                                 ended(false)
{
}

heapdiff::HeapDiff::~HeapDiff()
{
    if (before) {
        ((HeapSnapshot *) before)->Delete();
        before = NULL;
    }

    if (after) {
        ((HeapSnapshot *) after)->Delete();
        after = NULL;
    }
}

void
heapdiff::HeapDiff::Initialize ( v8::Handle<v8::Object> target )
{
    Isolate *isolate = Isolate::GetCurrent();
    v8::Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate,New);
    t->InstanceTemplate()->SetInternalFieldCount(1);
    t->SetClassName(String::NewFromUtf8(isolate, "HeapDiff"));

    NODE_SET_PROTOTYPE_METHOD(t, "end", End);

    target->Set(v8::String::NewFromUtf8( isolate, "HeapDiff"), t->GetFunction());
}

//v8::Handle<v8::Value>
void
heapdiff::HeapDiff::New (const v8::FunctionCallbackInfo<v8::Value>& args)
{
    Isolate *isolate = args.GetIsolate();
    // Don't blow up when the caller says "new require('memwatch').HeapDiff()"
    // issue #30
    // stolen from: https://github.com/kkaefer/node-cpp-modules/commit/bd9432026affafd8450ecfd9b49b7dc647b6d348
    if (!args.IsConstructCall()) {
        isolate->ThrowException(
            Exception::TypeError(
                String::NewFromUtf8(isolate,"Use the new operator to create instances of this object.")));
    }
    else {
      v8::HandleScope scope(isolate);

      // allocate the underlying c++ class and wrap it up in the this pointer
      HeapDiff * self = new HeapDiff();
      self->Wrap(args.This());

      // take a snapshot and save a pointer to it
      s_inProgress = true;
      s_startTime = time(NULL);
      v8::HeapProfiler *heapProfiler = isolate->GetHeapProfiler();
      self->before = heapProfiler->TakeHeapSnapshot(v8::String::NewFromUtf8(isolate,""), NULL, NULL );
      s_inProgress = false;

      args.GetReturnValue().Set( args.This() );
    }

}

static string handleToStr(const Handle<Value> & str)
{
	String::Utf8Value utfString(str->ToString());
	return *utfString;   
}

static void
buildIDSet(set<uint64_t> * seen, const HeapGraphNode* cur, int & s)
{
    Isolate *isolate = Isolate::GetCurrent();
    v8::HandleScope scope(isolate);

    // cycle detection
    if (seen->find(cur->GetId()) != seen->end()) {
        return;
    }
    // always ignore HeapDiff related memory
    if (cur->GetType() == HeapGraphNode::kObject &&
        handleToStr(cur->GetName()).compare("HeapDiff") == 0)
    {
        return;
    }

    // update memory usage as we go
    s += cur->GetSelfSize();

    seen->insert(cur->GetId());

    for (int i=0; i < cur->GetChildrenCount(); i++) {
        buildIDSet(seen, cur->GetChild(i)->GetToNode(), s);
    }
}


typedef set<uint64_t> idset;

// why doesn't STL work?
// XXX: improve this algorithm
void setDiff(idset a, idset b, vector<uint64_t> &c)
{
    for (idset::iterator i = a.begin(); i != a.end(); i++) {
        if (b.find(*i) == b.end()) c.push_back(*i);
    }
}


class example
{
public:
    HeapGraphEdge::Type context;
    HeapGraphNode::Type type;
    std::string name;
    std::string value;
    std::string heap_value;
    int self_size;
    int retained_size;
    int retainers;

    example() : context(HeapGraphEdge::kHidden),
                type(HeapGraphNode::kHidden),
                self_size(0), retained_size(0), retainers(0) { };
};


class change
{
public:
    long int size;
    long int added;
    long int released;
    std::vector<example> examples;

    change() : size(0), added(0), released(0) { }
};


typedef std::map<std::string, change>changeset;

static void manageChange(changeset & changes, const HeapGraphNode * node, bool added)
{
    std::string type;

    switch(node->GetType()) {
        case HeapGraphNode::kArray:
            type.append("Array");
            break;
        case HeapGraphNode::kString:
            type.append("String");
            break;
        case HeapGraphNode::kObject:
            type.append(handleToStr(node->GetName()));
            break;
        case HeapGraphNode::kCode:
            type.append("Code");
            break;
        case HeapGraphNode::kClosure:
            type.append("Closure");
            break;
        case HeapGraphNode::kRegExp:
            type.append("RegExp");
            break;
        case HeapGraphNode::kHeapNumber:
            type.append("Number");
            break;
        case HeapGraphNode::kNative:
            type.append("Native");
            break;
        case HeapGraphNode::kHidden:
        default:
            return;
    }

    if (changes.find(type) == changes.end()) {
        changes[type] = change();
    }

    changeset::iterator i = changes.find(type);

    i->second.size += node->GetSelfSize() * (added ? 1 : -1);
    if (added) i->second.added++;
    else i->second.released++;

    // XXX: example

    return;
}


static Handle<Value> changesetToObject(changeset & changes)
{
    Isolate *isolate = Isolate::GetCurrent();
    v8::EscapableHandleScope handleScope(isolate);
    Local<Array> a = Array::New(isolate);

    for (changeset::iterator i = changes.begin(); i != changes.end(); i++) {
        Local<Object> d = Object::New(isolate);
        d->Set(String::NewFromUtf8(isolate, "what"), String::NewFromUtf8(isolate, i->first.c_str()));
        d->Set(String::NewFromUtf8(isolate, "size_bytes"), Integer::New(isolate, i->second.size));
        d->Set(String::NewFromUtf8(isolate, "size"), String::NewFromUtf8(isolate,mw_util::niceSize(i->second.size).c_str()));
        d->Set(String::NewFromUtf8(isolate, "+"), Integer::New(isolate, i->second.added));
        d->Set(String::NewFromUtf8(isolate, "-"), Integer::New(isolate, i->second.released));
        a->Set(a->Length(), d);
    }

    return handleScope.Escape( a );

}



static v8::Handle<Value>
compare(const v8::HeapSnapshot * before, const v8::HeapSnapshot * after)
{
    Isolate *isolate = Isolate::GetCurrent();
    v8::EscapableHandleScope handleScope(isolate);
    int s, diffBytes;

    Local<Object> o = Object::New(isolate);

    // first let's append summary information
    Local<Object> b = Object::New(isolate);
    b->Set(String::NewFromUtf8(isolate, "nodes"), Integer::New(isolate, before->GetNodesCount()));
    b->Set(String::NewFromUtf8(isolate, "time"), NODE_UNIXTIME_V8(s_startTime));
    o->Set(String::NewFromUtf8(isolate, "before"), b);

    Local<Object> a = Object::New(isolate);
    a->Set(String::NewFromUtf8(isolate, "nodes"), Integer::New(isolate, after->GetNodesCount()));
    a->Set(String::NewFromUtf8(isolate, "time"), NODE_UNIXTIME_V8(time(NULL)));
    o->Set(String::NewFromUtf8(isolate, "after"), a);

    // now let's get allocations by name
    set<uint64_t> beforeIDs, afterIDs;
    s = 0;
    buildIDSet(&beforeIDs, before->GetRoot(), s);
    b->Set(String::NewFromUtf8(isolate, "size_bytes"), Integer::New(isolate, s));
    b->Set(String::NewFromUtf8(isolate, "size"), String::NewFromUtf8(isolate, mw_util::niceSize(s).c_str()));

    diffBytes = s;
    s = 0;
    buildIDSet(&afterIDs, after->GetRoot(), s);
    a->Set(String::NewFromUtf8(isolate, "size_bytes"), Integer::New(isolate, s));
    a->Set(String::NewFromUtf8(isolate, "size"), String::NewFromUtf8(isolate, mw_util::niceSize(s).c_str()));

    diffBytes = s - diffBytes;

    Local<Object> c = Object::New(isolate);
    c->Set(String::NewFromUtf8(isolate, "size_bytes"), Integer::New(isolate, diffBytes));
    c->Set(String::NewFromUtf8(isolate, "size"), String::NewFromUtf8(isolate, mw_util::niceSize(diffBytes).c_str()));
    o->Set(String::NewFromUtf8(isolate, "change"), c);

    // before - after will reveal nodes released (memory freed)
    vector<uint64_t> changedIDs;
    setDiff(beforeIDs, afterIDs, changedIDs);
    c->Set(String::NewFromUtf8(isolate, "freed_nodes"), Integer::New(isolate, changedIDs.size()));

    // here's where we'll collect all the summary information
    changeset changes;

    // for each of these nodes, let's aggregate the change information
    for (unsigned long i = 0; i < changedIDs.size(); i++) {
        const HeapGraphNode * n = before->GetNodeById(changedIDs[i]);
        manageChange(changes, n, false);
    }

    changedIDs.clear();

    // after - before will reveal nodes added (memory allocated)
    setDiff(afterIDs, beforeIDs, changedIDs);

    c->Set(String::NewFromUtf8(isolate, "allocated_nodes"), Integer::New(isolate, changedIDs.size()));

    for (unsigned long i = 0; i < changedIDs.size(); i++) {
        const HeapGraphNode * n = after->GetNodeById(changedIDs[i]);
        manageChange(changes, n, true);
    }

    c->Set(String::NewFromUtf8(isolate, "details"), changesetToObject(changes));

    //handleScope.Escape( o );
    return handleScope.Escape( o );
}

void
heapdiff::HeapDiff::End( const FunctionCallbackInfo<v8::Value>& args )
{
    Isolate *isolate = args.GetIsolate();
    // take another snapshot and compare them

    HeapDiff *t = ObjectWrap::Unwrap<HeapDiff>( args.Holder() );

    // How shall we deal with double .end()ing?  The only reasonable
    // approach seems to be an exception, cause nothing else makes
    // sense.
    if (t->ended) {
        //return v8::ThrowException(
        isolate->ThrowException(
            v8::Exception::Error(
                v8::String::NewFromUtf8(isolate, "attempt to end() a HeapDiff that was "
                                "already ended")));
    }
    else {

      t->ended = true;

      s_inProgress = true;

      t->after = isolate->GetHeapProfiler()->TakeHeapSnapshot(v8::String::NewFromUtf8(isolate, ""));
      s_inProgress = false;

      v8::Handle<Value> comparison = compare(t->before, t->after);
      // free early, free often.  I mean, after all, this process we're in is
      // probably having memory problems.  We want to help her.
      ((HeapSnapshot *) t->before)->Delete();
      t->before = NULL;
      ((HeapSnapshot *) t->after)->Delete();
      t->after = NULL;

      args.GetReturnValue().Set( comparison );
    }
}
