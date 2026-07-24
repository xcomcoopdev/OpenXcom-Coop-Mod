#pragma once
/*
 * Copyright 2010-2016 OpenXcom Developers.
 * Copyright 2023-2026 XComCoopTeam (https://www.moddb.com/mods/openxcom-coop-mod)
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <string>

namespace OpenXcom
{

/**
 * Small platform-independent clipboard facade.
 *
 * The platform backends return UTF-8 text. Keeping clipboard handling here
 * prevents TextEdit and the multiplayer menus from depending on Win32, X11,
 * or Cocoa APIs directly.
 */
class Clipboard
{
public:
	/// Returns clipboard text as UTF-8, or an empty string when unavailable.
	static std::string getText();
};

}
