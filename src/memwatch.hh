/*
 * 2012|lloyd|http://wtfpl.org
 */

#ifndef __MEMWATCH_HH
#define __MEMWATCH_HH

#include <node.h>

namespace memwatch
{
    //v8::Handle<v8::Value> upon_gc(const v8::FunctionCallbackInfo<v8::Value>& args);
    //v8::Handle<v8::Value> trigger_gc(const v8::FunctionCallbackInfo<v8::Value>& args);

    void upon_gc(const v8::FunctionCallbackInfo<v8::Value>& args);
    void trigger_gc(const v8::FunctionCallbackInfo<v8::Value>& args);
    void after_gc(v8::GCType type, v8::GCCallbackFlags flags);
};

#endif
