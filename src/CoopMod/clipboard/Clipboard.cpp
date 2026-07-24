/*
 * Copyright 2010-2016 OpenXcom Developers.
 * Copyright 2023-2026 XComCoopTeam (https://www.moddb.com/mods/openxcom-coop-mod)
 *
 * This file is part of OpenXcom.
 */

#include "Clipboard.h"
#include <cstdlib>

/*
 * Exactly one platform backend is compiled for each target. The backend
 * allocates the returned UTF-8 string with malloc(), and this common facade
 * takes ownership of it and frees it after copying it into std::string.
 */
extern "C" char *openxcomClipboardGetText();

namespace OpenXcom
{

std::string Clipboard::getText()
{
	char *text = openxcomClipboardGetText();
	if (!text)
	{
		return std::string();
	}

	const std::string result(text);
	std::free(text);
	return result;
}

}
