#pragma once
#include <string>

namespace CrashHandler
{
/// Install global handlers once at the very beginning of main().
void install();

/// Manual logging you can call from your own catch blocks.
void log(const std::string& message);
}

/// Helper macro: logs message + file + line.
#define CRASH_LOG(msg) \
	CrashHandler::log(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + (msg))
