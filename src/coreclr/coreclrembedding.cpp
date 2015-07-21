#include "edge.h"
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>
#include <set>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <libgen.h>

#if PLATFORM_DARWIN
#include <libproc.h>
#endif

#ifdef PLATFORM_DARWIN
const char* LIBCORECLR_NAME = "libcoreclr.dylib";
#else
const char* LIBCORECLR_NAME = "libcoreclr.so";
#endif

typedef HRESULT (*coreclr_create_delegateFunction)(
		void* hostHandle,
		unsigned int domainId,
		const char* assemblyName,
		const char* typeName,
		const char* methodName,
		void** delegate);
typedef HRESULT (*coreclr_initializeFunction)(
		const char *exePath,
		const char *appDomainFriendlyName,
		int propertyCount,
		const char** propertyKeys,
		const char** propertyValues,
		void** hostHandle,
		unsigned int* domainId);

unsigned int appDomainId;
void* hostHandle;
GetFuncFunction getFunc;
CallFuncFunction callFunc;
ContinueTaskFunction continueTask;
FreeHandleFunction freeHandle;
FreeMarshalDataFunction freeMarshalData;

#define CREATE_DELEGATE(functionName, functionPointer)\
	result = createDelegate(\
			hostHandle,\
			appDomainId,\
			"CoreCLREmbedding",\
			"CoreCLREmbedding",\
			functionName,\
			(void**) functionPointer);\
	\
	if (FAILED(result))\
	{\
		throwV8Exception("Call to coreclr_create_delegate() for %s failed with a return code of 0x%x.", functionName, result);\
		return result;\
	}\
	\
	DBG("CoreClrEmbedding::Initialize - CoreCLREmbedding.%s() loaded successfully", functionName);\

HRESULT CoreClrEmbedding::Initialize(BOOL debugMode)
{
    // Much of the CoreCLR bootstrapping process is cribbed from 
    // https://github.com/aspnet/dnx/blob/dev/src/dnx.coreclr.unix/dnx.coreclr.cpp

	DBG("CoreClrEmbedding::Initialize - Started")

    HRESULT result = S_OK;
    char currentDirectory[PATH_MAX];

    if (!getcwd(&currentDirectory[0], PATH_MAX))
    {
    	NanThrowError("Unable to get the current directory.");
        return E_FAIL;
    }

    Dl_info dlInfo;
	char edgeNodePath[PATH_MAX];

	dladdr((void*)&CoreClrEmbedding::Initialize, &dlInfo);
	strcpy(edgeNodePath, dlInfo.dli_fname);
	strcpy(edgeNodePath, dirname(edgeNodePath));

    void* libCoreClr = NULL;
    char bootstrapper[PATH_MAX];

    GetPathToBootstrapper(&bootstrapper[0], PATH_MAX);
    DBG("CoreClrEmbedding::Initialize - Bootstrapper is %s", bootstrapper);

    char coreClrDirectory[PATH_MAX];
    char* coreClrEnvironmentVariable = getenv("CORECLR_DIR");

    if (coreClrEnvironmentVariable)
    {
    	DBG("CoreClrEmbedding::Initialize - Trying to load %s from the path specified in the CORECLR_DIR environment variable: %s", LIBCORECLR_NAME, coreClrEnvironmentVariable);

    	strncpy(&coreClrDirectory[0], coreClrEnvironmentVariable, strlen(coreClrEnvironmentVariable) + 1);
        LoadCoreClrAtPath(coreClrDirectory, &libCoreClr);
    }

    if (!libCoreClr)
    {
    	strncpy(&coreClrDirectory[0], currentDirectory, strlen(currentDirectory) + 1);
    	LoadCoreClrAtPath(coreClrDirectory, &libCoreClr);
    }

    if (!libCoreClr)
    {
        // Try to load CoreCLR from application path
        char* lastSlash = strrchr(&bootstrapper[0], '/');

        assert(lastSlash);
        strncpy(&coreClrDirectory[0], &bootstrapper[0], lastSlash - &bootstrapper[0]);
        coreClrDirectory[lastSlash - &bootstrapper[0]] = '\0';

        LoadCoreClrAtPath(coreClrDirectory, &libCoreClr);
    }

    if (!libCoreClr)
    {
    	std::string pathEnvironmentVariable = getenv("PATH");
#if EDGE_PLATFORM_WINDOWS
    	char delimeter = ';';
#else
    	char delimeter = ':';
#endif

    	size_t previousIndex = 0;
    	size_t currentIndex = pathEnvironmentVariable.find(delimeter);

    	while (!libCoreClr && currentIndex != std::string::npos)
    	{
    		strncpy(&coreClrDirectory[0], pathEnvironmentVariable.substr(previousIndex, currentIndex - previousIndex).c_str(), currentIndex - previousIndex);
    		coreClrDirectory[currentIndex - previousIndex] = '\0';

    		LoadCoreClrAtPath(coreClrDirectory, &libCoreClr);

    		if (!libCoreClr)
    		{
				previousIndex = currentIndex + 1;
				currentIndex = pathEnvironmentVariable.find(delimeter, previousIndex);
    		}
    	}
    }

    if (!libCoreClr)
    {
    	NanThrowError("Failed to find CoreCLR.  Make sure that you have either specified the CoreCLR directory in the CORECLR_DIR environment variable or it exists somewhere in your PATH environment variable, which you do via the \"dnvm install\" and \"dnvm use\" commands.");
        return E_FAIL;
    }

    DBG("CoreClrEmbedding::Initialize - %s loaded successfully from %s", LIBCORECLR_NAME, &coreClrDirectory[0]);

    std::string assemblySearchDirectories;

    assemblySearchDirectories.append(&currentDirectory[0]);
    assemblySearchDirectories.append(":");
    assemblySearchDirectories.append(&coreClrDirectory[0]);

    DBG("CoreClrEmbedding::Initialize - Assembly search path is %s", assemblySearchDirectories.c_str());

    coreclr_initializeFunction initializeCoreCLR = (coreclr_initializeFunction) dlsym(libCoreClr, "coreclr_initialize");

    if (!initializeCoreCLR)
    {
    	throwV8Exception("Error loading the coreclr_initialize function from %s: %s.", LIBCORECLR_NAME, dlerror());
        return E_FAIL;
    }

    coreclr_create_delegateFunction createDelegate = (coreclr_create_delegateFunction) dlsym(libCoreClr, "coreclr_create_delegate");

    if (!createDelegate)
    {
    	throwV8Exception("Error loading the coreclr_create_delegate function from %s: %s.", LIBCORECLR_NAME, dlerror());
    	return E_FAIL;
    }

    const char* propertyKeys[] = {
    	"TRUSTED_PLATFORM_ASSEMBLIES",
    	"APP_PATHS",
    	"APP_NI_PATHS",
    	"NATIVE_DLL_SEARCH_DIRECTORIES",
    	"AppDomainCompatSwitch"
    };

    std::string tpaList;
    AddToTpaList(coreClrDirectory, &tpaList);

    std::string appPaths(&currentDirectory[0]);
    appPaths.append(":");
    appPaths.append(edgeNodePath);

    const char* propertyValues[] = {
    	tpaList.c_str(),
        appPaths.c_str(),
        appPaths.c_str(),
        assemblySearchDirectories.c_str(),
        "UseLatestBehaviorWhenTFMNotSpecified"
    };

    DBG("CoreClrEmbedding::Initialize - Calling coreclr_initialize()");
	result = initializeCoreCLR(
			bootstrapper,
			"Edge",
			sizeof(propertyKeys) / sizeof(propertyKeys[0]),
			&propertyKeys[0],
			&propertyValues[0],
			&hostHandle,
			&appDomainId);

	if (FAILED(result))
	{
		throwV8Exception("Call to coreclr_initialize() failed with a return code of 0x%x.", result);
		return result;
	}

	DBG("CoreClrEmbedding::Initialize - CoreCLR initialized successfully");
    DBG("CoreClrEmbedding::Initialize - App domain created successfully (app domain ID: %d)", appDomainId);

    SetDebugModeFunction setDebugMode;
    SetCallV8FunctionDelegateFunction setCallV8Function;

    CREATE_DELEGATE("GetFunc", &getFunc);
    CREATE_DELEGATE("CallFunc", &callFunc);
    CREATE_DELEGATE("ContinueTask", &continueTask);
    CREATE_DELEGATE("FreeHandle", &freeHandle);
    CREATE_DELEGATE("FreeMarshalData", &freeMarshalData);
    CREATE_DELEGATE("SetDebugMode", &setDebugMode);
    CREATE_DELEGATE("SetCallV8FunctionDelegate", &setCallV8Function);

    setDebugMode(debugMode);
	DBG("CoreClrEmbedding::Initialize - Debug mode set successfully");

	CoreClrGcHandle exception = NULL;
	setCallV8Function(CoreClrNodejsFunc::Call, &exception);

	if (exception)
	{
		Handle<v8::Value> v8Exception = CoreClrFunc::MarshalCLRToV8(exception, V8TypeException);
		FreeMarshalData(exception, V8TypeException);

		throwV8Exception(v8Exception);
	}

	else
	{
		DBG("CoreClrEmbedding::Initialize - CallV8Function delegate set successfully");
	}

	DBG("CoreClrEmbedding::Initialize - Completed");

    return S_OK;
}

CoreClrGcHandle CoreClrEmbedding::GetClrFuncReflectionWrapFunc(const char* assemblyFile, const char* typeName, const char* methodName, v8::Handle<v8::Value>* v8Exception)
{
	DBG("CoreClrEmbedding::GetClrFuncReflectionWrapFunc - Starting");

	CoreClrGcHandle exception;
	CoreClrGcHandle function = getFunc(assemblyFile, typeName, methodName, &exception);

	if (exception)
	{
		*v8Exception = CoreClrFunc::MarshalCLRToV8(exception, V8TypeException);
		FreeMarshalData(exception, V8TypeException);

		return NULL;
	}

	else
	{
		DBG("CoreClrEmbedding::GetClrFuncReflectionWrapFunc - Finished");
		return function;
	}
}

void CoreClrEmbedding::AddToTpaList(std::string directoryPath, std::string* tpaList)
{
    const char * const tpaExtensions[] = {
        ".ni.dll",
        ".dll",
        ".ni.exe",
        ".exe"
    };

    DIR* directory = opendir(directoryPath.c_str());
    std::set<std::string> addedAssemblies;

    // Walk the directory for each extension separately so that we first get files with .ni.dll extension,
    // then files with .dll extension, etc.
    for (uint extensionIndex = 0; extensionIndex < sizeof(tpaExtensions) / sizeof(tpaExtensions[0]); extensionIndex++)
    {
        const char* currentExtension = tpaExtensions[extensionIndex];
        int currentExtensionLength = strlen(currentExtension);

        struct dirent* directoryEntry;

        // For all entries in the directory
        while ((directoryEntry = readdir(directory)) != NULL)
        {
            // We are interested in files only
            switch (directoryEntry->d_type)
            {
                case DT_REG:
                    break;

                // Handle symlinks and file systems that do not support d_type
                case DT_LNK:
                case DT_UNKNOWN:
                    {
                        std::string fullFilename;

                        fullFilename.append(directoryPath);
                        fullFilename.append("/");
                        fullFilename.append(directoryEntry->d_name);

                        struct stat fileInfo;

                        if (stat(fullFilename.c_str(), &fileInfo) == -1)
                        {
                            continue;
                        }

                        if (!S_ISREG(fileInfo.st_mode))
                        {
                            continue;
                        }
                    }

                    break;

                default:
                    continue;
            }

            std::string filename(directoryEntry->d_name);

            // Check if the extension matches the one we are looking for
            int extensionPosition = filename.length() - currentExtensionLength;

            if ((extensionPosition <= 0) || (filename.compare(extensionPosition, currentExtensionLength, currentExtension) != 0))
            {
                continue;
            }

            std::string filenameWithoutExtension(filename.substr(0, extensionPosition));

            // Make sure if we have an assembly with multiple extensions present,
            // we insert only one version of it.
            if (addedAssemblies.find(filenameWithoutExtension) == addedAssemblies.end())
            {
                addedAssemblies.insert(filenameWithoutExtension);

                tpaList->append(directoryPath);
                tpaList->append("/");
                tpaList->append(filename);
                tpaList->append(":");
            }
        }

        // Rewind the directory stream to be able to iterate over it for the next extension
        rewinddir(directory);
    }

    closedir(directory);
}

void CoreClrEmbedding::FreeCoreClr(void** libCoreClr)
{
    if (libCoreClr)
    {
        dlclose(libCoreClr);
        libCoreClr = NULL;
    }
}

bool CoreClrEmbedding::LoadCoreClrAtPath(const char* loadPath, void** libCoreClrPointer)
{
    std::string coreClrDllPath(loadPath);

    DBG("CoreClrEmbedding::LoadCoreClrAtPath - Trying to load %s from %s", LIBCORECLR_NAME, loadPath);

    coreClrDllPath.append("/");
    coreClrDllPath.append(LIBCORECLR_NAME);

    *libCoreClrPointer = dlopen(coreClrDllPath.c_str(), RTLD_NOW | RTLD_GLOBAL);

    if (*libCoreClrPointer == NULL )
    {
        DBG("CoreClrEmbedding::LoadCoreClrAtPath - Errors loading %s from %s: %s", LIBCORECLR_NAME, loadPath, dlerror());
    }

    else
    {
    	DBG("CoreClrEmbedding::LoadCoreClrAtPath - Load of %s succeeded", LIBCORECLR_NAME);
    }

    return *libCoreClrPointer != NULL;
}

void CoreClrEmbedding::GetPathToBootstrapper(char* pathToBootstrapper, size_t bufferSize)
{
#ifdef PLATFORM_DARWIN
    ssize_t pathLength = proc_pidpath(getpid(), pathToBootstrapper, bufferSize);
#else
    ssize_t pathLength = readlink("/proc/self/exe", pathToBootstrapper, bufferSize);
#endif
    assert(pathLength > 0);

    // ensure pathToBootstrapper is null terminated, readlink for example
    // will not null terminate it.
    pathToBootstrapper[pathLength] = '\0';
}

void CoreClrEmbedding::CallClrFunc(CoreClrGcHandle functionHandle, void* payload, int payloadType, int* taskState, void** result, int* resultType)
{
	DBG("CoreClrEmbedding::CallClrFunc");
	callFunc(functionHandle, payload, payloadType, taskState, result, resultType);
}

void CoreClrEmbedding::ContinueTask(CoreClrGcHandle taskHandle, void* context, TaskCompleteFunction callback, void** exception)
{
	DBG("CoreClrEmbedding::ContinueTask");
	continueTask(taskHandle, context, callback, exception);
}

void CoreClrEmbedding::FreeHandle(CoreClrGcHandle handle)
{
	DBG("CoreClrEmbedding::FreeHandle");
	freeHandle(handle);
}

void CoreClrEmbedding::FreeMarshalData(void* marshalData, int marshalDataType)
{
	DBG("CoreClrEmbedding::FreeMarshalData");
	freeMarshalData(marshalData, marshalDataType);
}
