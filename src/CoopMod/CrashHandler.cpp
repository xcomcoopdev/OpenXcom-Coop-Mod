#include "CrashHandler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// ---- Minimal DbgHelp bits (no <DbgHelp.h> include) ----

typedef unsigned long ULONG;
typedef unsigned long long ULONG64;

typedef struct _SYMBOL_INFO
{
	ULONG SizeOfStruct;
	ULONG TypeIndex;
	ULONG64 Reserved[2];
	ULONG Index;
	ULONG Size;
	ULONG64 ModBase;
	ULONG Flags;
	ULONG64 Value;
	ULONG64 Address;
	ULONG Register;
	ULONG Scope;
	ULONG Tag;
	ULONG NameLen;
	ULONG MaxNameLen;
	CHAR Name[1];
} SYMBOL_INFO, *PSYMBOL_INFO;

typedef struct _IMAGEHLP_LINE64
{
	DWORD SizeOfStruct;
	PVOID Key;
	DWORD LineNumber;
	PCHAR FileName;
	DWORD64 Address;
} IMAGEHLP_LINE64, *PIMAGEHLP_LINE64;

// DbgHelp options flags (same values as in dbghelp.h)
#define SYMOPT_CASE_INSENSITIVE 0x00000001
#define SYMOPT_UNDNAME 0x00000002
#define SYMOPT_DEFERRED_LOADS 0x00000004
#define SYMOPT_LOAD_LINES 0x00000010

extern "C"
{
	DWORD WINAPI SymSetOptions(DWORD SymOptions);
	BOOL WINAPI SymInitialize(HANDLE hProcess, PCSTR UserSearchPath, BOOL fInvadeProcess);
	BOOL WINAPI SymFromAddr(HANDLE hProcess, DWORD64 Address, PDWORD64 Displacement, PSYMBOL_INFO Symbol);
	BOOL WINAPI SymGetLineFromAddr64(HANDLE hProcess, DWORD64 qwAddr, PDWORD pdwDisplacement, PIMAGEHLP_LINE64 Line64);
}

#pragma comment(lib, "Dbghelp.lib")

#else
// ---- POSIX (Linux, macOS, etc.) ----
#include <execinfo.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace
{
char g_logDir[1024] = {0};

#ifdef _WIN32
void initLogDir()
{
	if (g_logDir[0] != '\0')
		return;

	char exePath[MAX_PATH] = {0};
	DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
	if (len == 0 || len == MAX_PATH)
	{
		std::strcpy(g_logDir, "."); // fallback
		return;
	}

	char* lastSlash = std::strrchr(exePath, '\\');
	if (!lastSlash)
		lastSlash = std::strrchr(exePath, '/');
	if (lastSlash)
		*lastSlash = '\0';

	std::snprintf(g_logDir, sizeof(g_logDir), "%s\\crashlogs", exePath);
	CreateDirectoryA(g_logDir, nullptr); // OK if exists
}
#else
void initLogDir()
{
	if (g_logDir[0] != '\0')
		return;

	char cwd[PATH_MAX] = {0};
	if (!getcwd(cwd, sizeof(cwd)))
	{
		std::strcpy(g_logDir, ".");
	}
	else
	{
		std::snprintf(g_logDir, sizeof(g_logDir), "%s/crashlogs", cwd);
	}
	mkdir(g_logDir, 0755);
}
#endif

FILE* openCrashFile()
{
	initLogDir();

	std::time_t now = std::time(nullptr);
	std::tm* lt = std::localtime(&now);

	char filePath[1400];
#ifdef _WIN32
	std::snprintf(filePath, sizeof(filePath),
				  "%s\\crash_%04d%02d%02d_%02d%02d%02d.log",
				  g_logDir,
				  lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
				  lt->tm_hour, lt->tm_min, lt->tm_sec);
#else
	std::snprintf(filePath, sizeof(filePath),
				  "%s/crash_%04d%02d%02d_%02d%02d%02d.log",
				  g_logDir,
				  lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
				  lt->tm_hour, lt->tm_min, lt->tm_sec);
#endif

	FILE* f = std::fopen(filePath, "a");
	if (!f)
		return nullptr;

	std::fprintf(f, "==== Crash ====\n");
	std::fprintf(f, "Time: %s", std::asctime(lt));
	return f;
}

#ifdef _WIN32
bool g_symbolsInitialized = false;

void initSymbols()
{
	if (g_symbolsInitialized)
		return;

	HANDLE process = GetCurrentProcess();
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);

	if (SymInitialize(process, nullptr, TRUE))
		g_symbolsInitialized = true;
}

std::string formatAddress(void* addr)
{
	std::ostringstream oss;
	DWORD64 address = (DWORD64)addr;

	oss << "0x" << std::hex << address;

	initSymbols();
	if (!g_symbolsInitialized)
		return oss.str();

	HANDLE process = GetCurrentProcess();

	// symbol name
	char buffer[sizeof(SYMBOL_INFO) + 256] = {};
	PSYMBOL_INFO sym = (PSYMBOL_INFO)buffer;
	sym->SizeOfStruct = sizeof(SYMBOL_INFO);
	sym->MaxNameLen = 255;

	if (SymFromAddr(process, address, 0, sym))
	{
		oss << " " << sym->Name;
	}

	// file:line
	IMAGEHLP_LINE64 line;
	DWORD disp = 0;
	std::memset(&line, 0, sizeof(line));
	line.SizeOfStruct = sizeof(line);

	if (SymGetLineFromAddr64(process, address, &disp, &line))
	{
		oss << " (" << line.FileName << ":" << line.LineNumber << ")";
	}

	return oss.str();
}

LONG WINAPI vectoredHandler(PEXCEPTION_POINTERS info)
{
	FILE* f = openCrashFile();
	if (f)
	{
		void* addr = info->ExceptionRecord->ExceptionAddress;
		DWORD code = info->ExceptionRecord->ExceptionCode;

		std::string addrStr = formatAddress(addr);

		std::fprintf(
			f,
			"SEH exception. Code = 0x%08lX at %s\n",
			code,
			addrStr.c_str());
		std::fclose(f);
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

#else  // POSIX side

void writeStackTrace(FILE* f)
{
	void* frames[64];
	int n = backtrace(frames, 64);
	if (n <= 0)
		return;

	char** symbols = backtrace_symbols(frames, n);
	if (!symbols)
		return;

	std::fprintf(f, "\nStack trace (%d frames):\n", n);
	for (int i = 0; i < n; ++i)
	{
		std::fprintf(f, "#%02d %s\n", i, symbols[i]);
	}

	std::free(symbols);
}

void signalHandler(int sig)
{
	FILE* f = openCrashFile();
	if (f)
	{
		std::fprintf(f, "Received signal %d\n", sig);
		writeStackTrace(f);
		std::fclose(f);
	}
	_exit(1);
}
#endif // _WIN32 / POSIX

void terminateHandler()
{
	FILE* f = openCrashFile();
	if (!f)
	{
		std::abort();
	}

	try
	{
		std::exception_ptr p = std::current_exception();
		if (p)
		{
			try
			{
				std::rethrow_exception(p);
			}
			catch (const std::exception& e)
			{
				std::fprintf(f, "Unhandled C++ exception: %s\n", e.what());
			}
			catch (...)
			{
				std::fprintf(f, "Unhandled non-std C++ exception.\n");
			}
		}
		else
		{
			std::fprintf(f, "std::terminate called (no active exception).\n");
		}
	}
	catch (...)
	{
		std::fprintf(f, "Exception inside terminateHandler.\n");
	}

#ifndef _WIN32
	writeStackTrace(f);
#endif

	std::fclose(f);
	std::abort();
}
} // anonymous namespace

namespace CrashHandler
{
void install()
{
	initLogDir();

#ifdef _WIN32
	initSymbols();
	AddVectoredExceptionHandler(1, vectoredHandler);
#else
	signal(SIGSEGV, signalHandler);
	signal(SIGABRT, signalHandler);
#endif
	std::set_terminate(terminateHandler);
}

void log(const std::string& message)
{
	FILE* f = openCrashFile();
	if (!f)
		return;
	std::fprintf(f, "%s\n", message.c_str());
#ifndef _WIN32
	writeStackTrace(f);
#endif
	std::fclose(f);
}
}
