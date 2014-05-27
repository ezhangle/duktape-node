#include "run_sync.h"
#include "run_async.h"

#include <nan.h>
#include <node.h>

namespace {

using namespace v8;

// Main entrypoint
void init(Handle<Object> exports) 
{
	exports->Set(NanNew<String>("runSync"), NanNew<FunctionTemplate>(duktape::runSync)->GetFunction());
	exports->Set(NanNew<String>("run"), NanNew<FunctionTemplate>(duktape::runSync)->GetFunction());
}

} // unnamed namespace

NODE_MODULE(duktape, init)
