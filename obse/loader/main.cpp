#include "common/IFileStream.h"
#include "loader_common/EXEChecksum.h"
#include "loader_common/Options.h"
#include "Inject.h"
#include <string>

#include  <winhttp.h>

// requires recent platform sdk
#ifndef ERROR_ELEVATION_REQUIRED
#define ERROR_ELEVATION_REQUIRED 740
#endif
#include <obse/obse_common/obse_version.h>

IDebugLog	gLog("obse_loader.log");

static bool InjectDLL(PROCESS_INFORMATION * info, const char * dllPath, bool sync = true);
static bool DoInjectDLL(PROCESS_INFORMATION * info, const char * dllPath, bool sync);
static bool LoadOldblivion(PROCESS_INFORMATION * info);
#pragma comment(lib, "winhttp.lib")
int main(int argc, char ** argv)
{
	gLog.SetPrintLevel(IDebugLog::kLevel_Error);
	gLog.SetLogLevel(IDebugLog::kLevel_DebugMessage);

	if(!g_options.Read(argc, argv))
	{
		PrintError("Couldn't read arguments.");
		g_options.PrintUsage();

		return -1;
	}

	if(g_options.m_optionsOnly)
	{
		g_options.PrintUsage();
		return 0;
	}

	const char* releases = "https://api.github.com/repos/llde/xOBSE/releases/latest";
	//TODO Win 8.1 >= proxy flag
	HINTERNET gSession = WinHttpOpen(L"xOBSE Update", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (gSession) {
		HINTERNET gConnect = WinHttpConnect(gSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
		UInt32 pressed = 0;
		if (gConnect) {
			const WCHAR* type = L"application/json";
			const WCHAR* type1 = L"application/vnd.github+json";
			const WCHAR* accept[3] = { type , type1 , 0 };
			HINTERNET gRequest = WinHttpOpenRequest(gConnect, L"GET", L"/repos/llde/xOBSE/releases/latest", NULL, WINHTTP_NO_REFERER, accept, WINHTTP_FLAG_REFRESH | WINHTTP_FLAG_SECURE);
			if (gRequest) {
				BOOL res = WinHttpSendRequest(gRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
				if (res) {
					res = WinHttpReceiveResponse(gRequest, 0);
					if (res) {
						char* buffer = nullptr;
						DWORD read = 0;
						while (true) {
							DWORD bytes = 0;
							WinHttpQueryDataAvailable(gRequest, &bytes);
							if (bytes == 0) break;
							if (buffer == nullptr) buffer = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY ,bytes);
							else {
								char* oldBuf = buffer;
								buffer = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, read + bytes);
								memcpy(buffer,oldBuf, read);
								HeapFree(GetProcessHeap(), 0, oldBuf);
							}
							DWORD readi = 0;
							WinHttpReadData(gRequest, buffer + read, bytes, &readi);
							read += readi;
						}
						if (buffer != nullptr) {
							char* oldBuf = buffer;
							buffer = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, read + 1);
							memcpy(buffer, oldBuf, read);
							buffer[read] = '\0';
							HeapFree(GetProcessHeap(), 0, oldBuf);
							_MESSAGE("%s", buffer);
							std::string json = std::string(buffer);
							size_t tag = json.find("\"tag_name\":") + 12;
							size_t end = json.find("\"", tag);
							std::string sub = json.substr(tag, end - tag);
							size_t dot1 = sub.find(".");
							std::string major = sub.substr(0, dot1);
							std::string rest = sub.substr(dot1 + 1);
							size_t dot2 = rest.find(".");
							std::string minor, hotfix;
							if (dot2 != std::string::npos) {
								minor = rest.substr(0, dot2);
								hotfix = rest.substr(dot2, rest.size() - dot2);
							}
							else {
								minor = rest;
								hotfix = "0";
							}
							UInt32 Major = std::stoul(major);
							UInt32 Minor = std::stoul(minor);
							UInt32 Hotfix = std::stoul(hotfix);

							_MESSAGE("%u %u  %u ", Major, Minor, Hotfix);
							if (Major > OBSE_VERSION_INTEGER) {
								pressed = PrintOnScreen("New Major xOBSE Release available: get version %s from https://github.com/llde/xOBSE/releases \n Continue to load the game", sub.c_str());
							}
							else {
								if (Minor > OBSE_VERSION_INTEGER_MINOR) {
									pressed = PrintOnScreen("New Minor xOBSE Release available: get version %s from https://github.com/llde/xOBSE/releases \n Continue to load the game", sub.c_str());
								}
								else {
									if (Hotfix > OBSE_VERSION_INTEGER_HOTIFX) {
										pressed = PrintOnScreen("New Hotfix xOBSE Release available: get version %s from https://github.com/llde/xOBSE/releases. \n Continue to load the game", sub.c_str());
									}
								}
							}
							HeapFree(GetProcessHeap(), 0, buffer);
						}
					}
				}
				WinHttpCloseHandle(gRequest);
			}
			WinHttpCloseHandle(gConnect);
		}
		WinHttpCloseHandle(gSession);
		if (pressed == IDNO) return 0;
	}
	if(g_options.m_launchCS)
		_MESSAGE("launching editor");

	if(g_options.m_loadOldblivion)
		_MESSAGE("loading oldblivion");

	// create the process
	STARTUPINFO			startupInfo = { 0 };
	PROCESS_INFORMATION	procInfo = { 0 };
	bool				dllHasFullPath = false;

	startupInfo.cb = sizeof(startupInfo);

	const char	* procName = g_options.m_launchCS ? "TESConstructionSet.exe" : "Oblivion.exe";
	const char	* baseDllName = g_options.m_launchCS ? "obse_editor" : "obse";

	if(g_options.m_altEXE.size())
	{
		procName = g_options.m_altEXE.c_str();
		_MESSAGE("launching alternate exe (%s)", procName);
	}

	if(g_options.m_altDLL.size())
	{
		baseDllName = g_options.m_altDLL.c_str();
		_MESSAGE("launching alternate dll (%s)", baseDllName);

		dllHasFullPath = true;
	}

	std::string		dllSuffix;
	ProcHookInfo	procHookInfo;

	if(!TestChecksum(procName, &dllSuffix, &procHookInfo))
	{
		_ERROR("checksum not found");
		return -1;
	}

	if(procHookInfo.steamVersion)
	{
		// ### maybe check for the loader DLL and just CreateProcess("oblivion.exe") if we can?
		_MESSAGE("Launching Steam version of Oblivion. This method isn't tested and mayem with associated evocation of the saints of the months may occur.");
	}

	if(g_options.m_crcOnly)
		return 0;

	// build dll path
	std::string	dllPath;
	if(dllHasFullPath)
	{
		dllPath = baseDllName;
	}
	else
	{
		dllPath = GetCWD() + "\\" + baseDllName + "_" + dllSuffix + ".dll";
	}

	_MESSAGE("dll = %s", dllPath.c_str());

	// check to make sure the dll exists
	{
		IFileStream	tempFile;

		if(!tempFile.Open(dllPath.c_str()))
		{
			PrintError("Couldn't find OBSE DLL (%s). Please make sure you have installed OBSE correctly and are running it from your Oblivion folder.", dllPath.c_str());
			return -1;
		}
	}

	bool result = CreateProcess(
		procName,
		NULL,	// no args
		NULL,	// default process security
		NULL,	// default thread security
		TRUE,	// don't inherit handles
		CREATE_SUSPENDED,
		NULL,	// no new environment
		NULL,	// no new cwd
		&startupInfo, &procInfo) != 0;

	// check for Vista failing to create the process due to elevation requirements
	if(!result && (GetLastError() == ERROR_ELEVATION_REQUIRED))
	{
		// in theory we could figure out how to UAC-prompt for this process and then run CreateProcess again, but I have no way to test code for that
		PrintError("Vista has decided that launching Oblivion requires UAC privilege elevation. There is no good reason for this to happen, but to fix it, right-click on obse_loader.exe, go to Properties, pick the Compatibility tab, then turn on \"Run this program as an administrator\".");
		return -1;
	}
	
	ASSERT_STR_CODE(result, "Launching Oblivion failed", GetLastError());

	if(g_options.m_setPriority)
	{
		if(!SetPriorityClass(procInfo.hProcess, g_options.m_priority))
			_WARNING("couldn't set process priority");
	}

	result = false;

	if(g_options.m_launchCS)
	{
		if(g_options.m_oldInject)
		{
			_MESSAGE("using old editor injection method");

			// start the process
			ResumeThread(procInfo.hThread);

			// CS needs to run its crt0 code before the DLL is attached, this delays until the message pump is running
			// note that this method makes it impossible to patch the startup code

			// this is better than Sleep(1000) but still ugly
			WaitForInputIdle(procInfo.hProcess, 1000 * 10);

			// too late if this fails
			result = InjectDLL(&procInfo, dllPath.c_str(), !g_options.m_noSync);
			if(!result)
				PrintError("Couldn't inject dll.");
		}
		else
		{
			_MESSAGE("using new editor injection method");

			result = DoInjectDLL_New(&procInfo, dllPath.c_str(), &procHookInfo);
			if(!result)
				PrintError("Couldn't inject dll.");

			// start the process either way
			ResumeThread(procInfo.hThread);
		}
	}
	else
	{
		result = InjectDLL(&procInfo, dllPath.c_str(), !g_options.m_noSync);
		if(result)
		{
			// try to load oldblivion if requested
			if(g_options.m_loadOldblivion)
			{
				result = LoadOldblivion(&procInfo);
				if(!result)
					PrintError("Couldn't load oldblivion.");
			}
		}
		else
			PrintError("Couldn't inject dll.");
		
		if(result)
		{
			_MESSAGE("launching oblivion");

			// start the process
			ResumeThread(procInfo.hThread);
		}
		else
		{
			_ERROR("terminating oblivion process");

			// kill the partially-created process
			TerminateProcess(procInfo.hProcess, 0);

			g_options.m_waitForClose = false;
		}
	}

	// wait for the process to close if requested
	if(g_options.m_waitForClose)
	{
		WaitForSingleObject(procInfo.hProcess, INFINITE);
	}

	// clean up
	CloseHandle(procInfo.hProcess);
	CloseHandle(procInfo.hThread);

	return 0;
}

bool InjectDLL(PROCESS_INFORMATION * info, const char * dllPath, bool sync)
{
	bool	result = false;

	// wrap DLL injection in SEH, if it crashes print a message
	__try {
		result = DoInjectDLL(info, dllPath, sync);
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		PrintError("DLL injection failed. In most cases, this is caused by an overly paranoid software firewall or antivirus package. Disabling either of these may solve the problem.");
		result = false;
	}

	return result;
}

/*** jmp hook layout
 *	E9 ## ## ## ##	jmp LoadLibraryA
 *						offset = LoadLibraryA - (base + 5)
 *	<string>		name of function
 ***/

bool DoInjectDLL(PROCESS_INFORMATION * info, const char * dllPath, bool sync)
{
	bool	result = false;

	HANDLE	process = OpenProcess(
		PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, info->dwProcessId);
	if(process)
	{
		UInt32	hookBase = (UInt32)VirtualAllocEx(process, NULL, 8192, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if(hookBase)
		{
			// safe because kernel32 is loaded at the same address in all processes
			// (can change across restarts)
			UInt32	loadLibraryAAddr = (UInt32)GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryA");

			_MESSAGE("hookBase = %08X", hookBase);
			_MESSAGE("loadLibraryAAddr = %08X", loadLibraryAAddr);

			UInt32	bytesWritten;
			WriteProcessMemory(process, (LPVOID)(hookBase + 5), dllPath, strlen(dllPath) + 1, &bytesWritten);

			UInt8	hookCode[5];

			hookCode[0] = 0xE9;
			*((UInt32 *)&hookCode[1]) = loadLibraryAAddr - (hookBase + 5);

			WriteProcessMemory(process, (LPVOID)(hookBase), hookCode, sizeof(hookCode), &bytesWritten);

			HANDLE	thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)hookBase, (void *)(hookBase + 5), 0, NULL);
			if(thread)
			{
				if(sync)
				{
					switch(WaitForSingleObject(thread, g_options.m_threadTimeout))
					{
						case WAIT_OBJECT_0:
							_MESSAGE("hook thread complete");
							result = true;
							break;

						case WAIT_ABANDONED:
							_ERROR("Process::InstallHook: waiting for thread = WAIT_ABANDONED");
							break;

						case WAIT_TIMEOUT:
							_ERROR("Process::InstallHook: waiting for thread = WAIT_TIMEOUT");
							break;
					}
				}
				else
					result = true;

				CloseHandle(thread);
			}
			else
				_ERROR("CreateRemoteThread failed (%d)", GetLastError());

			VirtualFreeEx(process, (LPVOID)hookBase, 8192, MEM_RELEASE);
		}
		else
			_ERROR("Process::InstallHook: couldn't allocate memory in target process");

		CloseHandle(process);
	}
	else
		_ERROR("Process::InstallHook: couldn't get process handle");

	return result;
}

static bool LoadOldblivion(PROCESS_INFORMATION * info)
{
	bool	result = false;
	HANDLE	syncEvent = CreateEvent(NULL, 0, 0, "OldblivionInjectionCompleted");

	if(syncEvent)
	{
		if(InjectDLL(info, "oldblivion.dll", false))
		{
			switch(WaitForSingleObject(syncEvent, g_options.m_threadTimeout))
			{
				case WAIT_OBJECT_0:
					_MESSAGE("oldblivion loaded");
					result = true;
					break;

				case WAIT_ABANDONED:
					_ERROR("LoadOldblivion: waiting for oldblivion = WAIT_ABANDONED");
					break;

				case WAIT_TIMEOUT:
					_ERROR("LoadOldblivion: waiting for oldblivion = WAIT_TIMEOUT");
					break;
			}
		}
		else
			_ERROR("couldn't inject oldblivion dll");

		CloseHandle(syncEvent);
	}
	else
		_ERROR("couldn't create oldblivion sync event (%d)", GetLastError());

	return result;
}
