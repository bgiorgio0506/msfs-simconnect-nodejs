#include "addon.h"

#include <winternl.h>
#include <stack>

uv_loop_t *loop;
uv_async_t async;

std::map<DWORD, DataDefinition> dataDefinitions;
std::map<DWORD, Nan::Callback *> systemEventCallbacks;
std::map<DWORD, Nan::Callback *> systemStateCallbacks;
std::map<DWORD, Nan::Callback *> dataRequestCallbacks;
Nan::Callback *errorCallback;

// Special events to listen for from the beginning
SIMCONNECT_CLIENT_EVENT_ID openEventId;
SIMCONNECT_CLIENT_EVENT_ID quitEventId;
SIMCONNECT_CLIENT_EVENT_ID exceptionEventId;

// Counters for creating unique IDs for SimConnect
SIMCONNECT_DATA_DEFINITION_ID defineIdCounter;
SIMCONNECT_CLIENT_EVENT_ID eventIdCounter;
SIMCONNECT_DATA_REQUEST_ID requestIdCounter;

std::stack<SIMCONNECT_DATA_REQUEST_ID> unusedReqIds;

// Semaphores
uv_sem_t workerSem;
uv_sem_t defineIdSem;
uv_sem_t eventIdSem;
uv_sem_t reqIdSem;


HANDLE ghSimConnect = NULL;

class DispatchWorker : public Nan::AsyncWorker
{
public:
	DispatchWorker(Nan::Callback *callback) : AsyncWorker(callback)
	{
	}
	~DispatchWorker() {}

	void Execute()
	{
		uv_async_init(loop, &async, messageReceiver); // Must be called from worker thread

		while (true)
		{

			if (ghSimConnect)
			{
				uv_sem_wait(&workerSem); // Wait for mainthread to process the previous dispatch

				SIMCONNECT_RECV *pData;
				DWORD cbData;

				HRESULT hr = SimConnect_GetNextDispatch(ghSimConnect, &pData, &cbData);

				if (SUCCEEDED(hr))
				{
					CallbackData data;
					data.pData = pData;
					data.cbData = cbData;
					data.ntstatus = 0;
					async.data = &data;
					uv_async_send(&async);
				}
				else if (NT_ERROR(hr))
				{
					CallbackData data;
					data.ntstatus = (NTSTATUS)hr;
					async.data = &data;
					uv_async_send(&async);
				}
				else
				{
					uv_sem_post(&workerSem); // Continue
					Sleep(1);
				}
			}
			else
			{
				Sleep(10);
			}
		}
	}
};

SIMCONNECT_DATA_DEFINITION_ID getUniqueDefineId()
{
	uv_sem_wait(&defineIdSem);
	SIMCONNECT_DATA_DEFINITION_ID id = defineIdCounter;
	defineIdCounter++;
	uv_sem_post(&defineIdSem);
	return id;
}

SIMCONNECT_CLIENT_EVENT_ID getUniqueEventId()
{
	uv_sem_wait(&eventIdSem);
	SIMCONNECT_CLIENT_EVENT_ID id = eventIdCounter;
	eventIdCounter++;
	uv_sem_post(&eventIdSem);
	return id;
}

SIMCONNECT_DATA_REQUEST_ID getUniqueRequestId()
{
	uv_sem_wait(&reqIdSem);
	SIMCONNECT_DATA_REQUEST_ID id;
	if (!unusedReqIds.empty())
	{
		id = unusedReqIds.top();
		unusedReqIds.pop();
	}
	else
	{
		id = requestIdCounter;
		requestIdCounter++;
	}
	uv_sem_post(&reqIdSem);
	return id;
}

// Runs on main thread after uv_async_send() is called
void messageReceiver(uv_async_t *handle)
{

	Nan::HandleScope scope;
	v8::Isolate *isolate = v8::Isolate::GetCurrent();

	CallbackData *data = (CallbackData *)handle->data;

	if (NT_SUCCESS(data->ntstatus))
	{
		switch (data->pData->dwID)
		{
		case SIMCONNECT_RECV_ID_EVENT:
			handleReceived_Event(isolate, data->pData, data->cbData);
			break;
		case SIMCONNECT_RECV_ID_SIMOBJECT_DATA:
			handleReceived_Data(isolate, data->pData, data->cbData);
			break;
		case SIMCONNECT_RECV_ID_QUIT:
			handleReceived_Quit(isolate);
			break;
		case SIMCONNECT_RECV_ID_EXCEPTION:
			handleReceived_Exception(isolate, data->pData, data->cbData);
			break;
		case SIMCONNECT_RECV_ID_EVENT_FILENAME:
			handleReceived_Filename(isolate, data->pData, data->cbData);
			break;
		case SIMCONNECT_RECV_ID_OPEN:
			handleReceived_Open(isolate, data->pData, data->cbData);
			break;
		case SIMCONNECT_RECV_ID_SYSTEM_STATE:
			handleReceived_SystemState(isolate, data->pData, data->cbData);
			break;
		case SIMCONNECT_RECV_ID_SIMOBJECT_DATA_BYTYPE:
			handleReceived_DataByType(isolate, data->pData, data->cbData);
			break;
		case SIMCONNECT_RECV_ID_EVENT_FRAME:
			handleReceived_Frame(isolate, data->pData, data->cbData);
			break;
		default:
			printf("Unexpected message received (dwId: %i)\n", data->pData->dwID);
			break;
		}
	}
	else
	{
		handle_Error(isolate, data->ntstatus);
	}

	uv_sem_post(&workerSem); // The dispatch-worker can now continue
}

// Handles data requested with requestDataOnSimObject or requestDataOnSimObjectType
void handleReceived_Data(Isolate *isolate, SIMCONNECT_RECV *pData, DWORD cbData)
{
	SIMCONNECT_RECV_SIMOBJECT_DATA *pObjData = (SIMCONNECT_RECV_SIMOBJECT_DATA *)pData;
	int numVars = dataDefinitions[pObjData->dwDefineID].num_values;
	std::vector<SIMCONNECT_DATATYPE> valTypes = dataDefinitions[pObjData->dwDefineID].datum_types;
	std::vector<std::string> valIds = dataDefinitions[pObjData->dwDefineID].datum_names;

	v8::Local<v8::Context> ctx =  isolate->GetCurrentContext();

	Local<Object> result_list = Object::New(isolate);
	int dataValueOffset = 0;

	for (int i = 0; i < numVars; i++)
	{
		int varSize = 0;

		if (valTypes[i] == SIMCONNECT_DATATYPE_STRINGV)
		{
			dataValueOffset += 8; // Not really sure why this is needed, but it fixes problems like this: "F-22 RapF-22 Raptor - 525th Fighter Squadron"
			char *pOutString;
			DWORD cbString;
			char *pStringv = ((char *)(&pObjData->dwData));
			HRESULT hr = SimConnect_RetrieveString(pData, cbData, dataValueOffset + pStringv, &pOutString, &cbString);
			if (NT_ERROR(hr))
			{
				handle_Error(isolate, hr);
				return;
			}

			//New to local convertion
			v8::Local<v8::String> key;
			v8::MaybeLocal<v8::String> keyTemp = String::NewFromUtf8(isolate, valIds.at(i).c_str());
			keyTemp.ToLocal(&key);

			try
			{
				v8::Local<v8::String> value;
				v8::MaybeLocal<v8::String> valueTemp = String::NewFromOneByte(isolate, (const uint8_t *)pOutString, v8::NewStringType::kNormal);
				valueTemp.ToLocal(&value);
				result_list->Set(ctx, key, value);
			}
			catch (...)
			{
				v8::Local<v8::String> value;
				v8::MaybeLocal<v8::String> valueTemp = String::NewFromUtf8(isolate, "ERROR");
				valueTemp.ToLocal(&value);
				result_list->Set(ctx, key, value);
			}

			varSize = cbString;
		}
		else
		{
			//printf("------ %s -----\n", valIds.at(i).c_str());
			varSize = sizeMap[valTypes[i]];
			char *p = ((char *)(&pObjData->dwData) + dataValueOffset);
			double *var = (double *)p;
			v8::Local<v8::String> key;
			v8::MaybeLocal<v8::String> keytemp = String::NewFromUtf8(isolate, valIds.at(i).c_str());
			keytemp.ToLocal(&key);
			result_list->Set(ctx,key, Number::New(isolate, *var));
		}
		dataValueOffset += varSize;
	}

	const int argc = 1;
	Local<Value> argv[argc] = {
		result_list};
	dataRequestCallbacks[pObjData->dwRequestID]->Call(isolate->GetCurrentContext()->Global(), argc, argv);
}

void handleReceived_DataByType(Isolate *isolate, SIMCONNECT_RECV *pData, DWORD cbData)
{
	SIMCONNECT_RECV_SIMOBJECT_DATA *pObjData = (SIMCONNECT_RECV_SIMOBJECT_DATA *)pData;
	handleReceived_Data(isolate, pData, cbData);
	unusedReqIds.push(pObjData->dwRequestID); // The id can be re-used in next request
}

void handleReceived_Frame(Isolate *isolate, SIMCONNECT_RECV *pData, DWORD cbData)
{
	SIMCONNECT_RECV_EVENT_FRAME *pFrame = (SIMCONNECT_RECV_EVENT_FRAME *)pData;
	// printf("frame data recived: %f FPS\n",pFrame->fFrameRate);

	const int argc = 2;

	Local<Value> argv[argc] = {
		Number::New(isolate, pFrame->fFrameRate),
		Number::New(isolate, pFrame->fSimSpeed)};

	systemEventCallbacks[pFrame->uEventID]->Call(isolate->GetCurrentContext()->Global(), argc, argv);

	// Local<Object> obj = Object::New(isolate);

	// float   fFrameRate;
	// float   fSimSpeed;
	// DWORD   dwFlags;

	// obj->Set(String::NewFromUtf8(isolate, "float"), Number::New(isolate, pFrame->fFrameRate));
	// obj->Set(String::NewFromUtf8(isolate, "float"), Number::New(isolate, pFrame->fSimSpeed));

	// Local<Value> argv[1] = { obj };
	// unusedReqIds.push(pFrame->dwRequestID);	// The id can be re-used in next request
}

void handle_Error(Isolate *isolate, NTSTATUS code)
{
	// Codes found so far: 0xC000014B, 0xC000020D, 0xC000013C
	ghSimConnect = NULL;
	char errorCode[32];
	sprintf(errorCode, "0x%08X", code);

	const int argc = 1;

	//converting from maybeLocal to local
	v8::Local<v8::String> eleErrorCodeArgc;
	v8::MaybeLocal<v8::String> tempErrorCode = String::NewFromUtf8(isolate, errorCode);
	tempErrorCode.ToLocal(&eleErrorCodeArgc);
	Local<Value> argv[argc] = {eleErrorCodeArgc};

	errorCallback->Call(isolate->GetCurrentContext()->Global(), argc, argv);
}

void handleReceived_Event(Isolate *isolate, SIMCONNECT_RECV *pData, DWORD cbData)
{
	SIMCONNECT_RECV_EVENT *myEvent = (SIMCONNECT_RECV_EVENT *)pData;

	const int argc = 1;
	Local<Value> argv[argc] = {
		Number::New(isolate, myEvent->dwData)};

	systemEventCallbacks[myEvent->uEventID]->Call(isolate->GetCurrentContext()->Global(), argc, argv);
}

void handleReceived_Exception(Isolate *isolate, SIMCONNECT_RECV *pData, DWORD cbData)
{
	SIMCONNECT_RECV_EXCEPTION *except = (SIMCONNECT_RECV_EXCEPTION *)pData;

	v8::Local<v8::Context> ctx =  isolate->GetCurrentContext();

	v8::Local<v8::String> dwException;
	v8::Local<v8::String> dwSendID;
	v8::Local<v8::String> dwIndex;
	v8::Local<v8::String> cpData;
	v8::Local<v8::String> cpVersion;
	v8::Local<v8::String> name;
	v8::Local<v8::String> simConnExeption;

	v8::MaybeLocal<v8::String> dwExceptionTemp = String::NewFromUtf8(isolate, "dwException");
	v8::MaybeLocal<v8::String> dwSendIDTemp = String::NewFromUtf8(isolate, "dwSendID");
	v8::MaybeLocal<v8::String> dwIndexTemp = String::NewFromUtf8(isolate, "dwIndex");
	v8::MaybeLocal<v8::String> cpDataTemp = String::NewFromUtf8(isolate, "cbData");
	v8::MaybeLocal<v8::String> cpVersionTemp = String::NewFromUtf8(isolate, "cbVersion");
	v8::MaybeLocal<v8::String> nameTemp = String::NewFromUtf8(isolate, "name");
	v8::MaybeLocal<v8::String> simConnExeptionTemp = String::NewFromUtf8(isolate, exceptionNames[SIMCONNECT_EXCEPTION(except->dwException)]);

	dwExceptionTemp.ToLocal(&dwException);
	dwSendIDTemp.ToLocal(&dwSendID);
	dwIndexTemp.ToLocal(&dwIndex);
	cpDataTemp.ToLocal(&cpData);
	cpVersionTemp.ToLocal(&cpVersion);
	nameTemp.ToLocal(&name);
	simConnExeptionTemp.ToLocal(&simConnExeption);



	Local<Object> obj = Object::New(isolate);
	obj->Set(ctx ,dwException, Number::New(isolate, except->dwException));
	obj->Set(ctx ,dwSendID, Number::New(isolate, except->dwSendID));
	obj->Set(ctx ,dwIndex, Number::New(isolate, except->dwIndex));
	obj->Set(ctx ,cpData, Number::New(isolate, cbData));
	obj->Set(ctx ,cpVersion, Number::New(isolate, except->dwException));
	obj->Set(ctx ,name, simConnExeption);

	Local<Value> argv[1] = {obj};

	systemEventCallbacks[exceptionEventId]->Call(isolate->GetCurrentContext()->Global(), 1, argv);
}

void handleReceived_Filename(Isolate *isolate, SIMCONNECT_RECV *pData, DWORD cbData)
{
	SIMCONNECT_RECV_EVENT_FILENAME *fileName = (SIMCONNECT_RECV_EVENT_FILENAME *)pData;
	const int argc = 1;
	v8::Local<v8::String> eleArgc;
	v8::MaybeLocal<v8::String> TempEleArgc = String::NewFromUtf8(isolate, (const char *)fileName->szFileName);
	TempEleArgc.ToLocal(&eleArgc);
	Local<Value> argv[argc] = {eleArgc};

	systemEventCallbacks[fileName->uEventID]->Call(isolate->GetCurrentContext()->Global(), argc, argv);
}

void handleReceived_Open(Isolate *isolate, SIMCONNECT_RECV *pData, DWORD cbData)
{
	SIMCONNECT_RECV_OPEN *pOpen = (SIMCONNECT_RECV_OPEN *)pData;

	char simconnVersion[32];
	sprintf(simconnVersion, "%d.%d.%d.%d", pOpen->dwSimConnectVersionMajor, pOpen->dwSimConnectVersionMinor, pOpen->dwSimConnectBuildMajor, pOpen->dwSimConnectBuildMinor);

	const int argc = 2;
	//Convert  to MaybeLocal to local
	v8::Local<v8::String> simconnVersionArg;
	v8::MaybeLocal<v8::String> tempSimConn = String::NewFromUtf8(isolate, simconnVersion);
	tempSimConn.ToLocal(&simconnVersionArg);


	Local<Value> argv[argc] = {
		String::NewFromOneByte(isolate, (const uint8_t *)pOpen->szApplicationName, v8::NewStringType::kNormal).ToLocalChecked(),
		simconnVersionArg
		};

	systemEventCallbacks[openEventId]->Call(isolate->GetCurrentContext()->Global(), argc, argv);
}

void handleReceived_SystemState(Isolate *isolate, SIMCONNECT_RECV *pData, DWORD cbData)
{
	SIMCONNECT_RECV_SYSTEM_STATE *pState = (SIMCONNECT_RECV_SYSTEM_STATE *)pData;
	v8::Local<v8::Context> ctx =  isolate->GetCurrentContext();

	v8::Local<v8::String> integer;
	v8::Local<v8::String> floatt;
	v8::Local<v8::String> stringg;

	v8::MaybeLocal<v8::String> integerTemp = String::NewFromUtf8(isolate, "integer");
	v8::MaybeLocal<v8::String> floattemp = String::NewFromUtf8(isolate, "float");
	v8::MaybeLocal<v8::String> stringgTemp = String::NewFromUtf8(isolate, "string");

	integerTemp.ToLocal(&integer);
	floattemp.ToLocal(&floatt);
	stringgTemp.ToLocal(&stringg);
	

	Local<Object> obj = Object::New(isolate);
	obj->Set(ctx ,integer, Number::New(isolate, pState->dwInteger));
	obj->Set(ctx ,floatt, Number::New(isolate, pState->fFloat));
	obj->Set(ctx ,stringg, stringg);

	Local<Value> argv[1] = {obj};
	systemStateCallbacks[openEventId]->Call(isolate->GetCurrentContext()->Global(), 1, argv);
}

void handleReceived_Quit(Isolate *isolate)
{
	ghSimConnect = NULL;
	systemEventCallbacks[quitEventId]->Call(isolate->GetCurrentContext()->Global(), 0, NULL);
}

void handleSimDisconnect(Isolate *isolate)
{
}

// Wrapped SimConnect-functions //////////////////////////////////////////////////////
void Open(const v8::FunctionCallbackInfo<v8::Value> &args)
{
	uv_sem_init(&workerSem, 1);
	uv_sem_init(&defineIdSem, 1);
	uv_sem_init(&eventIdSem, 1);
	uv_sem_init(&reqIdSem, 1);

	defineIdCounter = 0;
	eventIdCounter = 0;
	requestIdCounter = 0;

	Isolate *isolate = args.GetIsolate();
	v8::Local<v8::Context> ctx = Nan::GetCurrentContext();

	// Get arguments
	Nan::Utf8String appName(args[0].As<String>());// Maybe wanted to Maybe<Local>
	//AppName = appName.ToLocalChecked();
	///char appName = *str

	openEventId = getUniqueEventId();
	systemEventCallbacks[openEventId] = {new Nan::Callback(args[1].As<Function>())};
	quitEventId = getUniqueEventId();
	systemEventCallbacks[quitEventId] = {new Nan::Callback(args[2].As<Function>())};
	exceptionEventId = getUniqueEventId();
	systemEventCallbacks[exceptionEventId] = {new Nan::Callback(args[3].As<Function>())};
	errorCallback = {new Nan::Callback(args[4].As<Function>())};

	// Create dispatch looper thread
	loop = uv_default_loop();

	Nan::AsyncQueueWorker(new DispatchWorker(NULL));

	// Open connection
	HRESULT hr = SimConnect_Open(&ghSimConnect, *appName, NULL, 0, 0, 0);

	// Return true if success
	Local<Boolean> retval = v8::Boolean::New(isolate, SUCCEEDED(hr));

	args.GetReturnValue().Set(retval);
}

void Close(const v8::FunctionCallbackInfo<v8::Value> &args)
{
	if (ghSimConnect)
	{
		Isolate *isolate = args.GetIsolate();
		printf("Trying to close..\n");
		HRESULT hr = SimConnect_Close(&ghSimConnect);
		if (NT_ERROR(hr))
		{
			handle_Error(isolate, hr);
			return;
		}

		printf("Closed: %i\n", hr);
		ghSimConnect = NULL;
		args.GetReturnValue().Set(v8::Boolean::New(isolate, SUCCEEDED(hr)));
	}
}

void isConnected(const v8::FunctionCallbackInfo<v8::Value> &args)
{
	Isolate *isolate = args.GetIsolate();
	args.GetReturnValue().Set(v8::Boolean::New(isolate, ghSimConnect));
}

void RequestSystemState(const v8::FunctionCallbackInfo<v8::Value> &args)
{
	if (ghSimConnect)
	{
		Isolate *isolate = args.GetIsolate();
		v8::Local<v8::Context> ctx = Nan::GetCurrentContext();

		Nan::Utf8String stateName(args[0].As<String>());

		SIMCONNECT_DATA_REQUEST_ID reqId = getUniqueRequestId();
		systemStateCallbacks[reqId] = new Nan::Callback(args[1].As<Function>());
		HRESULT hr = SimConnect_RequestSystemState(ghSimConnect, reqId, *stateName);
		if (NT_ERROR(hr))
		{
			handle_Error(isolate, hr);
			return;
		}
		args.GetReturnValue().Set(v8::Number::New(isolate, reqId));
	}
}

void FlightLoad(const v8::FunctionCallbackInfo<v8::Value> &args)
{
	if (ghSimConnect)
	{
		Isolate *isolate = args.GetIsolate();
		v8::Local<v8::Context> ctx = Nan::GetCurrentContext();

		Nan::Utf8String szFileName(args[0].As<String>()); //Maybe wanted to Maybe<Local>
		HRESULT hr = SimConnect_FlightLoad(ghSimConnect, *szFileName);
		if (NT_ERROR(hr))
		{
			handle_Error(isolate, hr);
			return;
		}
		args.GetReturnValue().Set(v8::Boolean::New(isolate, SUCCEEDED(hr)));
	}
}

void TransmitClientEvent(const v8::FunctionCallbackInfo<v8::Value> &args)
{
	if (ghSimConnect)
	{
		Isolate *isolate = args.GetIsolate();
		//v8::Local<v8::Context> ctx = Nan::GetCurrentContext();

		Nan::Utf8String eventName(args[0].As<String>()); // Maybe wanted to Maybe<Local>
		DWORD data = args.Length() > 1 ? args[1]->Int32Value(Nan::GetCurrentContext()).FromJust() : 0;

		SIMCONNECT_CLIENT_EVENT_ID id = getUniqueEventId();
		HRESULT hr = SimConnect_MapClientEventToSimEvent(ghSimConnect, id, *eventName);
		if (NT_ERROR(hr))
		{
			handle_Error(isolate, hr);
			return;
		}

		hr = SimConnect_TransmitClientEvent(ghSimConnect, SIMCONNECT_OBJECT_ID_USER, id, data, SIMCONNECT_GROUP_PRIORITY_HIGHEST, SIMCONNECT_EVENT_FLAG_GROUPID_IS_PRIORITY);
		if (NT_ERROR(hr))
		{
			handle_Error(isolate, hr);
			return;
		}

		args.GetReturnValue().Set(v8::Boolean::New(isolate, SUCCEEDED(hr)));
	}
}

void SubscribeToSystemEvent(const v8::FunctionCallbackInfo<v8::Value> &args)
{
	if (ghSimConnect)
	{
		v8::Isolate *isolate = args.GetIsolate();
		//v8::Local<v8::Context> ctx = Nan::GetCurrentContext();

		SIMCONNECT_CLIENT_EVENT_ID eventId = getUniqueEventId();

		Nan::Utf8String systemEventName(args[0].As<String>());//Maybe wanted to local checked
		systemEventCallbacks[eventId] = {new Nan::Callback(args[1].As<Function>())};

		HANDLE hSimConnect = ghSimConnect;
		HRESULT hr = SimConnect_SubscribeToSystemEvent(hSimConnect, eventId, *systemEventName);
		if (NT_ERROR(hr))
		{
			handle_Error(isolate, hr);
			return;
		}

		args.GetReturnValue().Set(v8::Integer::New(isolate, eventId));
	}
}

void RequestDataOnSimObject(const v8::FunctionCallbackInfo<v8::Value> &args)
{
	if (ghSimConnect)
	{
		v8::Isolate *isolate = args.GetIsolate();
		v8::Local<v8::Context> ctx = Nan::GetCurrentContext();

		Local<Array> reqValues = v8::Local<v8::Array>::Cast(args[0]);
		auto callback = new Nan::Callback(args[1].As<Function>());

		int objectId = args.Length() > 2 ? args[2]->Int32Value(Nan::GetCurrentContext()).FromJust() : SIMCONNECT_OBJECT_ID_USER;
		int periodId = args.Length() > 3 ? args[3]->Int32Value(Nan::GetCurrentContext()).FromJust() : SIMCONNECT_PERIOD_SIM_FRAME;
		int flags = args.Length() > 4 ? args[4]->Int32Value(Nan::GetCurrentContext()).FromJust() : 0;
		int origin = args.Length() > 5 ? args[5]->Int32Value(Nan::GetCurrentContext()).FromJust() : 0;
		int interval = args.Length() > 6 ? args[6]->Int32Value(Nan::GetCurrentContext()).FromJust() : 0;
		DWORD limit = args.Length() > 7 ? args[7]->NumberValue(Nan::GetCurrentContext()).FromJust() : 0;

		SIMCONNECT_DATA_REQUEST_ID reqId = getUniqueRequestId();

		DataDefinition definition = generateDataDefinition(isolate, ghSimConnect, reqValues);

		HRESULT hr = SimConnect_RequestDataOnSimObject(ghSimConnect, reqId, definition.id, objectId, SIMCONNECT_PERIOD(periodId), flags, origin, interval, limit);
		if (NT_ERROR(hr))
		{
			handle_Error(isolate, hr);
			return;
		}

		args.GetReturnValue().Set(v8::Boolean::New(isolate, SUCCEEDED(hr)));

		dataDefinitions[definition.id] = definition;
		dataRequestCallbacks[reqId] = callback;
	}
}

void RequestDataOnSimObjectType(const v8::FunctionCallbackInfo<v8::Value> &args)
{
	if (ghSimConnect)
	{
		v8::Isolate *isolate = args.GetIsolate();
		//v8::Local<v8::Context> ctx = Nan::GetCurrentContext();

		DataDefinition definition;

		if (args[0]->IsArray())
		{
			Local<Array> reqValues = v8::Local<v8::Array>::Cast(args[0]);
			definition = generateDataDefinition(isolate, ghSimConnect, reqValues);
		}
		else if (args[0]->IsNumber())
		{
			definition = dataDefinitions[args[0]->NumberValue(Nan::GetCurrentContext()).FromJust()];
		}

		auto callback = new Nan::Callback(args[1].As<Function>());

		DWORD radius = args.Length() > 2 ? args[2]->Int32Value(Nan::GetCurrentContext()).FromJust() : 0;
		int typeId = args.Length() > 3 ? args[3]->Int32Value(Nan::GetCurrentContext()).FromJust() : SIMCONNECT_SIMOBJECT_TYPE_USER;

		SIMCONNECT_DATA_REQUEST_ID reqId = getUniqueRequestId();
		HRESULT hr = SimConnect_RequestDataOnSimObjectType(ghSimConnect, reqId, definition.id, radius, SIMCONNECT_SIMOBJECT_TYPE(typeId));
		if (NT_ERROR(hr))
		{
			handle_Error(isolate, hr);
			return;
		}

		args.GetReturnValue().Set(v8::Boolean::New(isolate, SUCCEEDED(hr)));

		dataDefinitions[definition.id] = definition;
		dataRequestCallbacks[reqId] = callback;
	}
}

void CreateDataDefinition(const v8::FunctionCallbackInfo<v8::Value> &args)
{
	if (ghSimConnect)
	{
		v8::Isolate *isolate = args.GetIsolate();
		Local<Array> reqValues = v8::Local<v8::Array>::Cast(args[0]);
		DataDefinition definition = generateDataDefinition(isolate, ghSimConnect, reqValues);
		args.GetReturnValue().Set(v8::Number::New(isolate, definition.id));
		dataDefinitions[definition.id] = definition;
	}
}

void SetDataOnSimObject(const v8::FunctionCallbackInfo<v8::Value> &args)
{
	if (ghSimConnect)
	{
		v8::Isolate *isolate = args.GetIsolate();
		v8::Local<v8::Context> ctx = Nan::GetCurrentContext();

		Nan::Utf8String name(args[0].As<String>());
		Nan::Utf8String unit(args[1].As<String>());

		double value = args[2]->NumberValue(Nan::GetCurrentContext()).FromJust();

		int objectId = args.Length() > 3 ? args[3]->Int32Value(Nan::GetCurrentContext()).FromMaybe(SIMCONNECT_OBJECT_ID_USER) : SIMCONNECT_OBJECT_ID_USER;
		int flags = args.Length() > 4 ? args[4]->Int32Value(Nan::GetCurrentContext()).FromJust() : 0;

		SIMCONNECT_DATA_DEFINITION_ID defId = getUniqueDefineId();

		HRESULT hr = SimConnect_AddToDataDefinition(ghSimConnect, defId, *name, *unit);
		if (NT_ERROR(hr))
		{
			handle_Error(isolate, hr);
			return;
		}

		hr = SimConnect_SetDataOnSimObject(ghSimConnect, defId, SIMCONNECT_OBJECT_ID_USER, 0, 0, sizeof(value), &value);
		if (NT_ERROR(hr))
		{
			handle_Error(isolate, hr);
			return;
		}

		args.GetReturnValue().Set(v8::Boolean::New(isolate, SUCCEEDED(hr)));
	}
}

// Generates a SimConnect data definition for the collection of requests.
DataDefinition generateDataDefinition(Isolate *isolate, HANDLE hSimConnect, Local<Array> requestedValues)
{

	SIMCONNECT_DATA_DEFINITION_ID definitionId = getUniqueDefineId();
	v8::Local<v8::Context> ctx = Nan::GetCurrentContext(); // use for all

	HRESULT hr = -1;
	bool success = true;
	unsigned int numValues = requestedValues->Length();

	std::vector<std::string> datumNames;
	std::vector<SIMCONNECT_DATATYPE> datumTypes;

	for (unsigned int i = 0; i < requestedValues->Length(); i++)
	{
		Local<Array> value = v8::Local<v8::Array>::Cast(requestedValues->Get(ctx,i).ToLocalChecked());

		if (value->IsArray())
		{
			int len = value->Length();

			if (len > 1)
			{
				Nan::Utf8String datumName(value->Get(ctx, 0).ToLocalChecked());
				const char *sDatumName = *datumName;
				const char *sUnitsName = NULL;

				if (!value->Get(ctx, 1).ToLocalChecked()->IsNull())
				{ // Should be NULL for string
					Nan::Utf8String unitsName(value->Get(ctx, 1).ToLocalChecked().As<String>());
					sUnitsName = *unitsName;
				}

				SIMCONNECT_DATATYPE datumType = SIMCONNECT_DATATYPE_FLOAT64; // Default type (double)
				double epsilon;
				float datumId;

				if (len > 1)
				{
					hr = SimConnect_AddToDataDefinition(hSimConnect, definitionId, sDatumName, sUnitsName);
					if (NT_ERROR(hr))
					{
						handle_Error(isolate, hr);
						break;
					}
				}
				if (len > 2)
				{
					int t = value->Get(ctx, 2).ToLocalChecked()->Int32Value(Nan::GetCurrentContext()).FromJust();
					datumType = SIMCONNECT_DATATYPE(t);
					hr = SimConnect_AddToDataDefinition(hSimConnect, definitionId, sDatumName, sUnitsName, datumType);
					if (NT_ERROR(hr))
					{
						handle_Error(isolate, hr);
						break;
					}
				}
				if (len > 3)
				{
					epsilon = value->Get(ctx, 3).ToLocalChecked()->Int32Value(Nan::GetCurrentContext()).FromJust();
					hr = SimConnect_AddToDataDefinition(hSimConnect, definitionId, sDatumName, sUnitsName, datumType, epsilon);
					if (NT_ERROR(hr))
					{
						handle_Error(isolate, hr);
						break;
					}
				}
				if (len > 4)
				{
					datumId = value->Get(ctx, 4).ToLocalChecked()->Int32Value(Nan::GetCurrentContext()).FromJust();
					hr = SimConnect_AddToDataDefinition(hSimConnect, definitionId, sDatumName, sUnitsName, datumType, epsilon, datumId);
					if (NT_ERROR(hr))
					{
						handle_Error(isolate, hr);
						break;
					}
				}

				std::string datumNameStr(sDatumName);
				datumNames.push_back(datumNameStr);
				datumTypes.push_back(datumType);
			}
		}
	}

	return {definitionId, numValues, datumNames, datumTypes};
}

// Custom useful functions ////////////////////////////////////////////////////////////////////
void SetAircraftInitialPosition(const v8::FunctionCallbackInfo<v8::Value> &args)
{
	if (ghSimConnect)
	{
		Isolate *isolate = args.GetIsolate();
		v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

		SIMCONNECT_DATA_INITPOSITION init;

		//Nan::GetCurrentContext()
		Local<Object> json = args[0]->ToObject(Nan::GetCurrentContext()).ToLocalChecked();

		v8::Local<v8::String> altProp = Nan::New("altitude").ToLocalChecked();
		v8::Local<v8::String> latProp = Nan::New("latitude").ToLocalChecked();
		v8::Local<v8::String> lngProp = Nan::New("longitude").ToLocalChecked();
		v8::Local<v8::String> pitchProp = Nan::New("pitch").ToLocalChecked();
		v8::Local<v8::String> bankProp = Nan::New("bank").ToLocalChecked();
		v8::Local<v8::String> hdgProp = Nan::New("heading").ToLocalChecked();
		v8::Local<v8::String> gndProp = Nan::New("onGround").ToLocalChecked();
		v8::Local<v8::String> iasProp = Nan::New("airspeed").ToLocalChecked();

		init.Altitude = json->HasRealNamedProperty(Nan::GetCurrentContext(), altProp).FromJust()? json->Get(ctx , altProp).ToLocalChecked()->NumberValue(Nan::GetCurrentContext()).FromJust() : 0;
		init.Latitude = json->HasRealNamedProperty(Nan::GetCurrentContext(), latProp).FromJust() ? json->Get(ctx, latProp).ToLocalChecked()->NumberValue(Nan::GetCurrentContext()).FromJust() : 0;
		init.Longitude = json->HasRealNamedProperty(Nan::GetCurrentContext(), lngProp).FromJust() ? json->Get(ctx, lngProp).ToLocalChecked()->NumberValue(Nan::GetCurrentContext()).FromJust() : 0;
		init.Pitch = json->HasRealNamedProperty(Nan::GetCurrentContext(), pitchProp).FromJust() ? json->Get(ctx, pitchProp).ToLocalChecked()->NumberValue(Nan::GetCurrentContext()).FromJust() : 0;
		init.Bank = json->HasRealNamedProperty(Nan::GetCurrentContext(), bankProp).FromJust() ? json->Get(ctx, bankProp).ToLocalChecked()->NumberValue(Nan::GetCurrentContext()).FromJust() : 0;
		init.Heading = json->HasRealNamedProperty(Nan::GetCurrentContext(), hdgProp).FromJust() ? json->Get(ctx, hdgProp).ToLocalChecked()->NumberValue(Nan::GetCurrentContext()).FromJust() : 0;
		init.OnGround = json->HasRealNamedProperty(Nan::GetCurrentContext(), gndProp).FromJust() ? json->Get(ctx, gndProp).ToLocalChecked()->IntegerValue(Nan::GetCurrentContext()).FromJust() : 0;
		init.Airspeed = json->HasRealNamedProperty(Nan::GetCurrentContext(), iasProp).FromJust() ? json->Get(ctx, iasProp).ToLocalChecked()->IntegerValue(Nan::GetCurrentContext()).FromJust() : 0;

		SIMCONNECT_DATA_DEFINITION_ID id = getUniqueDefineId();
		HRESULT hr = SimConnect_AddToDataDefinition(ghSimConnect, id, "Initial Position", NULL, SIMCONNECT_DATATYPE_INITPOSITION);
		if (NT_ERROR(hr))
		{
			handle_Error(isolate, hr);
			return;
		}

		hr = SimConnect_SetDataOnSimObject(ghSimConnect, id, SIMCONNECT_OBJECT_ID_USER, 0, 0, sizeof(init), &init);
		if (NT_ERROR(hr))
		{
			handle_Error(isolate, hr);
			return;
		}

		args.GetReturnValue().Set(v8::Boolean::New(isolate, SUCCEEDED(hr)));
	}
}

void Initialize(v8::Local<v8::Object> exports)
{
	NODE_SET_METHOD(exports, "open", Open);
	NODE_SET_METHOD(exports, "close", Close);
	NODE_SET_METHOD(exports, "subscribeToSystemEvent", SubscribeToSystemEvent);
	NODE_SET_METHOD(exports, "requestDataOnSimObject", RequestDataOnSimObject);
	NODE_SET_METHOD(exports, "setDataOnSimObject", SetDataOnSimObject);
	NODE_SET_METHOD(exports, "requestDataOnSimObjectType", RequestDataOnSimObjectType);
	NODE_SET_METHOD(exports, "setAircraftInitialPosition", SetAircraftInitialPosition);
	NODE_SET_METHOD(exports, "transmitClientEvent", TransmitClientEvent);
	NODE_SET_METHOD(exports, "requestSystemState", RequestSystemState);
	NODE_SET_METHOD(exports, "createDataDefinition", CreateDataDefinition);
	NODE_SET_METHOD(exports, "flightLoad", FlightLoad);
	NODE_SET_METHOD(exports, "isConnected", isConnected);
}

NODE_MODULE(addon, Initialize);
