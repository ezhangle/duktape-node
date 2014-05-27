#include "run_sync.h"

#include "callback.h"
#include "duktapevm.h"
	
using namespace v8;

namespace {

struct CallbackHelper
{
	CallbackHelper(Local<Function> apiCallbackFunc):
	m_apiCallbackFunc(apiCallbackFunc)
	{
	}

	std::string operator()(const std::string& paramString)
	{
		Handle<Value> argv[1];
		argv[0] = NanNew<String>(paramString.c_str());

		auto retVal = m_apiCallbackFunc->Call(NanGetCurrentContext()->Global(), 1, argv);

		String::Utf8Value retString(retVal);

		return std::string(*retString);
	}

private:
	Local<Function> m_apiCallbackFunc;
};

}

namespace duktape {

NAN_METHOD(runSync)
{
	NanEscapableScope();
	if(args.Length() < 3) 
	{
		NanThrowError(Exception::TypeError(NanNew<String>("Wrong number of arguments")));
		NanReturnUndefined();
	}

	if (!args[0]->IsString() || !args[1]->IsString() || !args[2]->IsString()) 
	{
		NanThrowError(Exception::TypeError(NanNew<String>("Wrong arguments")));
		NanReturnUndefined();
	}

	duktape::DuktapeVM vm;

	if(args[3]->IsObject())
	{
		auto object = Handle<Object>::Cast(args[3]);
		auto properties = object->GetPropertyNames();

		const auto len = properties->Length();
		for(unsigned int i = 0; i < len; ++i)
		{
			const Local<Value> key = properties->Get(i);
			const Local<Value> value = object->Get(key);
			if(!key->IsString() || !value->IsFunction())
			{
				NanThrowError(Exception::Error(NanNew<String>("Error in API-definition")));
				NanReturnUndefined();
			}

			Local<Function> apiCallbackFunc = Local<Function>::Cast(value);

			auto duktapeToNodeBridge = duktape::Callback(CallbackHelper(apiCallbackFunc));

			String::Utf8Value keyStr(key);
			vm.registerCallback(std::string(*keyStr), duktapeToNodeBridge);
		}		
	}

 	String::Utf8Value functionName(args[0]->ToString());
	String::Utf8Value parameters(args[1]->ToString());
	String::Utf8Value script(args[2]->ToString());

	auto ret = vm.run(std::string(*functionName), std::string(*parameters), std::string(*script));

	if(ret.errorCode != 0)
	{
		NanThrowError(Exception::Error(NanNew<String>(ret.value.c_str())));
		NanReturnUndefined();
	}
	NanReturnValue(NanNew<String>(ret.value.c_str()));
}

} // namespace duktape
