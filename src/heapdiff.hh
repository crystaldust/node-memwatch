/*
 * 2012|lloyd|http://wtfpl.org
 */

#ifndef __HEADDIFF_H
#define __HEADDIFF_H

#include <v8.h>
#include <v8-profiler.h>
#include <node.h>
#include <node_object_wrap.h>

namespace heapdiff 
{
    class HeapDiff : public node::ObjectWrap
    {
      public:
        static void Initialize ( v8::Handle<v8::Object> target );

        static void New( const v8::FunctionCallbackInfo<v8::Value>& info );
        static void End( const v8::FunctionCallbackInfo<v8::Value>& info );
        static bool InProgress();

      protected:
        HeapDiff();
        ~HeapDiff();
      private:
        const v8::HeapSnapshot * before;
        const v8::HeapSnapshot * after;
        bool ended;
    };
};

#endif
