#include "CrashHandler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <sstream>
#include <string>

#include "../version.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

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
std::atomic<unsigned> g_fileSeq{0};

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

static void localtime_safe(std::time_t t, std::tm& outTm)
{
#ifdef _WIN32
	localtime_s(&outTm, &t);
#else
	localtime_r(&t, &outTm);
#endif
}

// Build a "crash_<timestamp>_<seq>.<ext>" path inside the crash-log dir. The
// same stamp is reused for a crash's .log and .dmp so they pair up on disk.
void makeCrashPath(char* out, size_t n, unsigned seq, const char* ext)
{
	initLogDir();

	const auto now = std::chrono::system_clock::now();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
	const std::time_t tt = std::chrono::system_clock::to_time_t(now);
	std::tm lt{};
	localtime_safe(tt, lt);

	const char sep =
#ifdef _WIN32
		'\\';
#else
		'/';
#endif

	std::snprintf(
		out, n,
		"%s%ccrash_%04d%02d%02d_%02d%02d%02d_%03lld_%u.%s",
		g_logDir, sep,
		lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
		lt.tm_hour, lt.tm_min, lt.tm_sec,
		(long long)ms.count(),
		seq, ext);
}

#ifdef _WIN32
// Emit a build-identity block so an address in this log can be tied back to the
// exact binary (and thus its PDB). ASLR is off (RandomizedBaseAddress=false),
// so ExceptionAddress = ImageBase + RVA; logging the base + the PE link
// timestamp lets a dev pick the matching build and compute the RVA offline.
void writeBuildIdentity(FILE* f)
{
	std::fprintf(f, "Version: %s%s\n", OPENXCOM_VERSION_SHORT, OPENXCOM_VERSION_GIT);
	std::fprintf(f, "Compiled: %s %s\n", __DATE__, __TIME__);

	HMODULE mod = GetModuleHandleA(nullptr);
	if (!mod)
		return;

	char modPath[MAX_PATH] = {0};
	GetModuleFileNameA(mod, modPath, MAX_PATH);

	// PE header -> link TimeDateStamp + SizeOfImage (fingerprint the binary).
	DWORD timeStamp = 0, imageSize = 0;
	auto* dos = (IMAGE_DOS_HEADER*)mod;
	if (dos->e_magic == IMAGE_DOS_SIGNATURE)
	{
		auto* nt = (IMAGE_NT_HEADERS*)((BYTE*)mod + dos->e_lfanew);
		if (nt->Signature == IMAGE_NT_SIGNATURE)
		{
			timeStamp = nt->FileHeader.TimeDateStamp;
			imageSize = nt->OptionalHeader.SizeOfImage;
		}
	}

	std::fprintf(f, "Module: %s\n", modPath);
	std::fprintf(f, "ImageBase: 0x%llX  SizeOfImage: 0x%lX  PE-TimeDateStamp: 0x%08lX\n",
		(unsigned long long)(DWORD64)mod, (unsigned long)imageSize, (unsigned long)timeStamp);
}

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

// Format an address as "module+0xRVA symbolName (file:line) [0xABSOLUTE]".
// The module+RVA form is ASLR-independent and resolvable offline against the
// matching PDB/map; the symbol/line parts only appear when a PDB is present.
std::string formatAddress(void* addr)
{
	std::ostringstream oss;
	DWORD64 address = (DWORD64)addr;

	// module + RVA (works with no PDB at all)
	HMODULE mod = nullptr;
	if (GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)addr, &mod) && mod)
	{
		char modPath[MAX_PATH] = {0};
		GetModuleFileNameA(mod, modPath, MAX_PATH);
		const char* base = std::strrchr(modPath, '\\');
		base = base ? base + 1 : modPath;
		oss << base << "+0x" << std::hex << (address - (DWORD64)mod);
	}
	else
	{
		oss << "0x" << std::hex << address;
	}

	initSymbols();
	if (g_symbolsInitialized)
	{
		HANDLE process = GetCurrentProcess();

		char buffer[sizeof(SYMBOL_INFO) + 256] = {};
		PSYMBOL_INFO sym = (PSYMBOL_INFO)buffer;
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 symDisp = 0;
		if (SymFromAddr(process, address, &symDisp, sym))
			oss << " " << sym->Name << "+0x" << std::hex << symDisp;

		IMAGEHLP_LINE64 line;
		DWORD disp = 0;
		std::memset(&line, 0, sizeof(line));
		line.SizeOfStruct = sizeof(line);
		if (SymGetLineFromAddr64(process, address, &disp, &line))
			oss << " (" << line.FileName << ":" << std::dec << line.LineNumber << ")";
	}

	oss << " [0x" << std::hex << address << "]";
	return oss.str();
}

// Walk the faulting thread using its captured CONTEXT (StackWalk64), rather than
// RtlCaptureStackBackTrace on the handler's own stack. This yields the real
// crash frames, including those below the exception dispatcher.
void writeStackWalk(FILE* f, const CONTEXT* ctxIn)
{
	if (!ctxIn)
		return;

	initSymbols();

	CONTEXT ctx = *ctxIn; // StackWalk64 mutates the context
	HANDLE process = GetCurrentProcess();
	HANDLE thread = GetCurrentThread();

	STACKFRAME64 frame;
	std::memset(&frame, 0, sizeof(frame));
	DWORD machine;
#if defined(_M_X64)
	machine = IMAGE_FILE_MACHINE_AMD64;
	frame.AddrPC.Offset = ctx.Rip;
	frame.AddrFrame.Offset = ctx.Rbp;
	frame.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_IX86)
	machine = IMAGE_FILE_MACHINE_I386;
	frame.AddrPC.Offset = ctx.Eip;
	frame.AddrFrame.Offset = ctx.Ebp;
	frame.AddrStack.Offset = ctx.Esp;
#else
	machine = IMAGE_FILE_MACHINE_UNKNOWN;
#endif
	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrFrame.Mode = AddrModeFlat;
	frame.AddrStack.Mode = AddrModeFlat;

	std::fprintf(f, "Stack (faulting thread):\n");
	for (int i = 0; i < 64; ++i)
	{
		if (!StackWalk64(machine, process, thread, &frame, &ctx,
				nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
			break;
		if (frame.AddrPC.Offset == 0)
			break;
		std::fprintf(f, "  #%d %s\n", i, formatAddress((void*)frame.AddrPC.Offset).c_str());
	}
}

// Write a minidump next to the .log so it can be opened in VS/WinDbg with the
// matching PDB for a fully symbolized stack + locals.
void writeMiniDump(unsigned seq, PEXCEPTION_POINTERS info)
{
	char dumpPath[1400];
	makeCrashPath(dumpPath, sizeof(dumpPath), seq, "dmp");

	HANDLE file = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;

	MINIDUMP_EXCEPTION_INFORMATION mei;
	mei.ThreadId = GetCurrentThreadId();
	mei.ExceptionPointers = info;
	mei.ClientPointers = FALSE;

	// Normal (stacks + thread contexts + module list) is enough to symbolize the
	// crash; IndirectlyReferencedMemory pulls in what stack pointers point at
	// (locals' objects) for a few MB more, rather than the tens of MB that
	// WithDataSegs would add by dumping every global.
	const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(
		MiniDumpNormal | MiniDumpWithThreadInfo |
		MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);

	MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
		info ? &mei : nullptr, nullptr, nullptr);

	CloseHandle(file);
}

// A fatal SEH may unwind through a noexcept frame into std::terminate instead of
// reaching SetUnhandledExceptionFilter, so more than one handler can race to dump.
// Write exactly one minidump - from whichever handler first sees the crash (the
// VEH, which still holds the real faulting CONTEXT).
std::atomic<bool> g_dumpWritten{false};
void tryWriteMiniDumpOnce(unsigned seq, PEXCEPTION_POINTERS info)
{
	bool expected = false;
	if (g_dumpWritten.compare_exchange_strong(expected, true))
		writeMiniDump(seq, info);
}

// Exception codes that mean the process is going down (as opposed to first-chance
// exceptions the app catches and handles).
static bool isFatalException(DWORD code)
{
	switch (code)
	{
	case 0xC0000005: // access violation
	case 0xC00000FD: // stack overflow
	case 0xC000001D: // illegal instruction
	case 0xC0000094: // integer divide by zero
	case 0xC0000096: // privileged instruction
	case 0xC0000025: // noncontinuable exception
	case 0xC0000026: // invalid disposition
		return true;
	default:
		return false;
	}
}
#endif // _WIN32

FILE* openCrashFile()
{
	const unsigned seq = g_fileSeq.fetch_add(1, std::memory_order_relaxed);

	char filePath[1400];
	makeCrashPath(filePath, sizeof(filePath), seq, "log");

	FILE* f = std::fopen(filePath, "a");
	if (!f)
		return nullptr;

	const auto now = std::chrono::system_clock::now();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
	const std::time_t tt = std::chrono::system_clock::to_time_t(now);
	std::tm lt{};
	localtime_safe(tt, lt);

	char timeBuf[64];
	std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &lt);

	std::fprintf(f, "==== Crash/Log ====\n");
	std::fprintf(f, "Time: %s.%03lld\n", timeBuf, (long long)ms.count());
#ifdef _WIN32
	writeBuildIdentity(f);
#else
	std::fprintf(f, "Version: %s%s\n", OPENXCOM_VERSION_SHORT, OPENXCOM_VERSION_GIT);
	std::fprintf(f, "Compiled: %s %s\n", __DATE__, __TIME__);
#endif
	return f;
}

#ifdef _WIN32
// This crash's seq is shared between the .log and the .dmp; grab it up front.
static unsigned nextSeqPeek()
{
	return g_fileSeq.load(std::memory_order_relaxed);
}

static bool isErrorSeverity(DWORD code)
{
	// NTSTATUS severity: 00=success, 01=informational, 10=warning, 11=error
	return ((code >> 30) == 3);
}

static bool isNoiseException(DWORD code)
{
	switch (code)
	{
	case 0x40010006:
		return true; // DBG_PRINTEXCEPTION_C (OutputDebugString / DBGPRINT)
	case 0x406D1388:
		return true; // MSVC thread naming exception
	case 0x80000003:
		return true; // breakpoint
	case 0x80000004:
		return true; // single-step
	case 0xE06D7363:
		return true; // MSVC C++ EH (thrown-and-caught std/other exceptions - noise first-chance)
	default:
		return false;
	}
}

LONG WINAPI vectoredHandler(PEXCEPTION_POINTERS info)
{
	const DWORD code = info->ExceptionRecord->ExceptionCode;

	// IMPORTANT: Don't treat debug/info/warning exceptions as crashes.
	// VEH sees *everything* (first-chance too).
	if (isNoiseException(code) || !isErrorSeverity(code))
		return EXCEPTION_CONTINUE_SEARCH;

	const unsigned seq = nextSeqPeek();
	FILE* f = openCrashFile();
	if (f)
	{
		void* addr = info->ExceptionRecord->ExceptionAddress;
		std::fprintf(f, "SEH exception (VEH, first-chance). Code = 0x%08lX at %s\n",
			code, formatAddress(addr).c_str());
		writeStackWalk(f, info->ContextRecord);
		std::fclose(f);
	}

	// Capture a minidump here, while we still hold the faulting CONTEXT - the
	// exception may otherwise reach std::terminate rather than unhandledFilter.
	if (isFatalException(code))
		tryWriteMiniDumpOnce(seq, info);

	return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI unhandledFilter(PEXCEPTION_POINTERS info)
{
	// This runs when the process is actually going to crash (unhandled exception).
	const unsigned seq = nextSeqPeek();

	FILE* f = openCrashFile();
	if (f)
	{
		const DWORD code = info->ExceptionRecord->ExceptionCode;
		void* addr = info->ExceptionRecord->ExceptionAddress;
		std::fprintf(f, "UNHANDLED SEH (fatal). Code = 0x%08lX at %s\n",
			code, formatAddress(addr).c_str());
		writeStackWalk(f, info->ContextRecord);
		std::fclose(f);
	}

	tryWriteMiniDumpOnce(seq, info);

	return EXCEPTION_EXECUTE_HANDLER;
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

	bool hadActiveException = false;
	try
	{
		std::exception_ptr p = std::current_exception();
		if (p)
		{
			hadActiveException = true;
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

#ifdef _WIN32
	// Backstop dump only for a real unhandled C++ exception. A "no active
	// exception" std::terminate is how the game exits normally, so dumping there
	// would litter a minidump on every clean shutdown.
	if (hadActiveException)
		tryWriteMiniDumpOnce(g_fileSeq.load(std::memory_order_relaxed), nullptr);
#else
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

	// Log only real crashes as "unhandled"
	SetUnhandledExceptionFilter(unhandledFilter);

	// Optional: keep VEH for extra visibility, but now filtered (won't spam with 0x40010006).
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
