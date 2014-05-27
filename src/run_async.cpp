#include "run_async.h"

#include "duktapevm.h"
#include "callback.h"

#include <nan.h>
#include <node.h>

#include <string>
#include <vector>

using namespace v8;
using node::FatalException;

namespace {

// Forward declaration for APICallbackSignaling destructor.
void cleanupUvAsync(uv_handle_s* handle);

// Forward declaration for CallbackHelper.
void callV8FunctionOnMainThread(uv_async_t* handle);

struct WorkRequest
{
	WorkRequest(std::string functionName, std::string parameters, std::string script, NanCallback* callback):
	 functionName(std::move(functionName))
	,parameters(std::move(parameters))
	,script(std::move(script))
	,callback(callback)
	,hasError(false)
	,returnValue()
	{
	};

	~WorkRequest()
	{
		for(auto it = apiCallbackFunctions.begin(); it != apiCallbackFunctions.end(); ++it)
		{
			delete *it;
			//NanDisposePersistent(*it);
			//it->Dispose();
			// TODO: it->Clear();
		}
		delete callback;
		//NanDisposePersistent(callback);
		// TODO:callback.Clear();
	}

	duktape::DuktapeVM vm;
	std::vector< NanCallback* > apiCallbackFunctions;

	// in
	std::string functionName;
	std::string parameters;
	std::string script;

	// out
	NanCallback* callback;
	bool hasError;
	std::string returnValue;
};

struct ScopedUvWorkRequest
{
	ScopedUvWorkRequest(uv_work_t* work):
	 m_work(work)
	,m_workRequest(static_cast<WorkRequest*> (m_work->data))
	{
	}

	~ScopedUvWorkRequest()
	{
		delete m_workRequest;
		delete m_work;
	}

	WorkRequest* getWorkRequest()
	{
		return m_workRequest;
	}

private:
	uv_work_t* m_work;
	WorkRequest* m_workRequest;
};

struct APICallbackSignaling
{	
	APICallbackSignaling(NanCallback* callback, std::string parameter, uv_async_cb cbFunc):
	 callback(callback)
	,parameter(parameter)
	,returnValue("")
	,done(false)
	,cbFunc(cbFunc)
	,async(new uv_async_t)
	{
		uv_mutex_init(&mutex);
		uv_cond_init(&cv);
		uv_async_init(uv_default_loop(), async, cbFunc);	
	}

	~APICallbackSignaling()
	{
		uv_mutex_destroy(&mutex);
		uv_cond_destroy(&cv);
		uv_close((uv_handle_t*) async, &cleanupUvAsync);
	}

	NanCallback* callback;
	std::string parameter;
	std::string returnValue;

	bool done;

	uv_cond_t cv;
	uv_mutex_t mutex;
	uv_async_cb cbFunc;

	// Has to be on heap, because of closing logic.
	uv_async_t* async;
};

struct CallbackHelper
{
	CallbackHelper(NanCallback* persistentApiCallbackFunc):
	m_persistentApiCallbackFunc(persistentApiCallbackFunc)
	{
	}

	std::string operator()(const std::string& paramString)
	{
		// We're on not on libuv/V8 main thread. Signal main to run 
		// callback function and wait for an answer.
		APICallbackSignaling cbsignaling(m_persistentApiCallbackFunc, 
										paramString,
										callV8FunctionOnMainThread);
						
		uv_mutex_lock(&cbsignaling.mutex);

		cbsignaling.async->data = (void*) &cbsignaling;
		uv_async_send(cbsignaling.async);
		while(!cbsignaling.done)
		{
			uv_cond_wait(&cbsignaling.cv, &cbsignaling.mutex);			
		}
		std::string retStr(cbsignaling.returnValue);

		uv_mutex_unlock(&cbsignaling.mutex);

		return retStr;
	}

private:
	NanCallback* m_persistentApiCallbackFunc;
};

void cleanupUvAsync(uv_handle_s* handle)
{
	// "handle" is "async"-parameter passed to uv_close
	delete (uv_async_t*) handle;
}

void callV8FunctionOnMainThread(uv_async_t* handle) 
{
	auto signalData = static_cast<APICallbackSignaling*> (handle->data);
	uv_mutex_lock(&signalData->mutex);

	NanScope();
	Handle<Value> argv[1];
	argv[0] = NanNew<String>(signalData->parameter.c_str());
	auto retVal = signalData->callback->GetFunction()->Call(NanGetCurrentContext()->Global(), 1, argv);

	size_t dummy;
	signalData->returnValue = std::string(NanCString(retVal, &dummy));

	signalData->done = true;
	uv_mutex_unlock(&signalData->mutex);
	uv_cond_signal(&signalData->cv);
}

void onWork(uv_work_t* req)
{
	// Do not use scoped-wrapper as req is still needed in onWorkDone.
	WorkRequest* work = static_cast<WorkRequest*> (req->data);

	auto ret = work->vm.run(work->functionName, work->parameters, work->script);
	work->hasError = ret.errorCode != 0;
	work->returnValue = ret.value;
}

void onWorkDone(uv_work_t* req, int status)
{
	ScopedUvWorkRequest uvReq(req);
	WorkRequest* work = uvReq.getWorkRequest();

	NanScope();

	Handle<Value> argv[2];
	argv[0] = NanNew<Boolean>(work->hasError);
	argv[1] = NanNew<String>(work->returnValue.c_str());

	TryCatch try_catch;
	work->callback->GetFunction()->Call(NanGetCurrentContext()->Global(), 2, argv);

	if (try_catch.HasCaught()) 
	{
		FatalException(try_catch);
	}
}

} // unnamed namespace

namespace duktape {

NAN_METHOD(run)
{
	NanScope();
	if(args.Length() < 5) 
	{
		NanThrowError(Exception::TypeError(NanNew<String>("Wrong number of arguments")));
		NanReturnUndefined();
	}

	if (!args[0]->IsString() || !args[1]->IsString() || !args[2]->IsString() || !args[4]->IsFunction()) 
	{
		NanThrowError(Exception::TypeError(NanNew<String>("Wrong arguments")));
		NanReturnUndefined();
	}

	String::Utf8Value functionName(args[0]->ToString());
	String::Utf8Value parameters(args[1]->ToString());
	String::Utf8Value script(args[2]->ToString());

	auto returnCallback = Handle<Function>::Cast(args[4]);
	auto nanCallback = new NanCallback(returnCallback);

	WorkRequest* workReq = new WorkRequest(	std::string(*functionName), 
											std::string(*parameters), 
											std::string(*script), 
											nanCallback);

	// API Handling
	if(args[3]->IsObject())
	{
		auto object = Handle<Object>::Cast(args[3]);
		auto properties = object->GetPropertyNames();

		auto len = properties->Length();
		for(unsigned int i = 0; i < len; ++i)
		{
			Local<Value> key = properties->Get(i);
			Local<Value> value = object->Get(key);
			if(!key->IsString() || !value->IsFunction())
			{
				NanThrowError(Exception::Error(NanNew<String>("Error in API-definition")));
				NanReturnUndefined();
			}
			auto apiCallbackFunc = Handle<Function>::Cast(value);
			auto callback = new NanCallback(apiCallbackFunc);
			auto duktapeToNodeBridge = duktape::Callback(CallbackHelper(callback));

			// Switch ownership of Persistent-Function to workReq
			workReq->apiCallbackFunctions.push_back(callback);

			size_t dummy;
			workReq->vm.registerCallback(std::string(NanCString(key, &dummy)), duktapeToNodeBridge);
		}
	}

	uv_work_t* req = new uv_work_t();
	req->data = workReq;

	uv_queue_work(uv_default_loop(), req, onWork, onWorkDone);

	NanReturnUndefined();
}

} // namespace duktape
