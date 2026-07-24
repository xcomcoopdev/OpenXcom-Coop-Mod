/*
 * Copyright 2010-2016 OpenXcom Developers.
 * Copyright 2023-2026 XComCoopTeam (https://www.moddb.com/mods/openxcom-coop-mod)
 *
 * This file is part of OpenXcom.
 */

#ifdef _WIN32

#include <windows.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <cstdlib>
#include <cstring>

namespace
{

HWND getOpenXcomWindow()
{
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if (SDL_GetWMInfo(&info))
	{
		return info.window;
	}
	return 0;
}

char *wideToUtf8(const wchar_t *wide)
{
	if (!wide)
	{
		return 0;
	}

	const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, 0, 0, 0, 0);
	if (bytes <= 0)
	{
		return 0;
	}

	char *result = static_cast<char *>(std::malloc(static_cast<size_t>(bytes)));
	if (!result)
	{
		return 0;
	}

	if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, result, bytes, 0, 0))
	{
		std::free(result);
		return 0;
	}

	return result;
}

char *ansiToUtf8(const char *ansi)
{
	if (!ansi)
	{
		return 0;
	}

	const int wideLength = MultiByteToWideChar(CP_ACP, 0, ansi, -1, 0, 0);
	if (wideLength <= 0)
	{
		return 0;
	}

	wchar_t *wide = static_cast<wchar_t *>(
		std::malloc(static_cast<size_t>(wideLength) * sizeof(wchar_t)));
	if (!wide)
	{
		return 0;
	}

	char *result = 0;
	if (MultiByteToWideChar(CP_ACP, 0, ansi, -1, wide, wideLength))
	{
		result = wideToUtf8(wide);
	}

	std::free(wide);
	return result;
}

}

extern "C" char *openxcomClipboardGetText()
{
	if (!OpenClipboard(getOpenXcomWindow()))
	{
		return 0;
	}

	char *result = 0;

	// Prefer Unicode. These APIs are available on Windows XP as well.
	HANDLE data = GetClipboardData(CF_UNICODETEXT);
	if (data)
	{
		const wchar_t *wide = static_cast<const wchar_t *>(GlobalLock(data));
		if (wide)
		{
			result = wideToUtf8(wide);
			GlobalUnlock(data);
		}
	}

	// Some older applications only publish CF_TEXT. Convert it through the
	// current Windows ANSI code page instead of treating it as UTF-8.
	if (!result)
	{
		data = GetClipboardData(CF_TEXT);
		if (data)
		{
			const char *ansi = static_cast<const char *>(GlobalLock(data));
			if (ansi)
			{
				result = ansiToUtf8(ansi);
				GlobalUnlock(data);
			}
		}
	}

	CloseClipboard();
	return result;
}

#endif
