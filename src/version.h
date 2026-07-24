#pragma once
/*
 * Copyright 2010-2016 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */

#define MIN_REQUIRED_RULESET_VERSION_NUMBER 8,4,1,0

// Co-op save schema version. Bumped whenever the on-disk co-op save format
// changes; every save writes it into the header as "saveSchema". The in-game
// Save Upgrader (src/Savegame/Upgrade/) detects older schemas and migrates
// them up to this current value one step at a time. See PRD save-upgrader.md.
#define SAVE_SCHEMA_CURRENT 2

#define OPENXCOM_VERSION_ENGINE "Extended"
#define OPENXCOM_VERSION_SHORT "Extended 8.4.2"
#define OPENXCOM_VERSION_LONG "8.4.2.0"
#define OPENXCOM_VERSION_NUMBER 8,4,2,0

#ifndef OPENXCOM_VERSION_GIT
#define OPENXCOM_VERSION_GIT " (v2025-10-06)"
#endif
