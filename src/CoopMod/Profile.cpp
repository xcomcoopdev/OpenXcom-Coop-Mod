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
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>. 
 */
#include "Profile.h"
#include "../Engine/Action.h"
#include "../Engine/Collections.h"
#include "../Engine/FileMap.h"
#include "../Engine/Game.h"
#include "../Engine/InteractiveSurface.h"
#include "../Engine/LocalizedText.h"
#include "../Engine/Options.h"
#include "../Engine/Palette.h"
#include "../Engine/Screen.h"
#include "../Engine/Sound.h"
#include "../Engine/Surface.h"
#include "../Engine/SurfaceSet.h"
#include "../Interface/Text.h"
#include "../Interface/TextEdit.h"
#include <algorithm>
#include "../Interface/Window.h"

namespace OpenXcom
{

Profile::Profile()
{

	int x = 50;

	// Create objects
	_bg = new Surface(320, 200, 0, 0);
	_txtName = new Text(210, 17, 30 + 18, 130);
	_avatar = new ImageButton(64, 64, x + 70, 50);
	_btnOk = new TextButton(180, 18, x + 18, 152);

	_btnOk->setText("OK");
	std::string playerName = _game->getCoopMod()->getCurrentClientName();

	std::string result = "ERROR";

	if (_game->getCoopMod()->getHost() == true)
	{
		result = playerName + " has joined the game";
	}
	else
	{
		result = "You have joined " + playerName + "'s game";
	}

	_txtName->setText(result);

	_txtName->setAlign(ALIGN_CENTER);
	_txtName->setBig();
	_txtName->setHighContrast(true);

	// Set palette
	setStandardPalette("PAL_BATTLESCAPE");

	add(_bg);

	add(_txtName, "textName", "inventory", _bg);
	add(_btnOk, "buttonOK", "inventory", _bg);
	add(_avatar, "textName", "inventory", _bg);

	centerAllSurfaces();

	_btnOk->onMouseClick((ActionHandler)&Profile::buttonOK);

	// load profile image
	try
	{
		if (_avatar)
		{
			Surface a;
			a.loadImage("multiplayer/avatar.png");
			a.blitNShade(_avatar, 0, 0);
		}
	}
	catch (const std::exception& e)
	{
		DebugLog(std::string("Failed to load avatar image: ") + e.what());
	}
	catch (...)
	{
		DebugLog("Failed to load avatar image: unknown error");
	}

}

void Profile::buttonOK(Action *)
{
	_game->popState();

	// save progress
	if (_game->getCoopMod()->getServerOwner() == false && connectionTCP::_host_save_progress == true && connectionTCP::saveID != 0)
	{

		_game->pushState(new CoopState(52));

		Json::Value root;

		root["state"] = "request_load_progress";

		_game->getCoopMod()->sendTCPPacketData(root.toStyledString());

	}

}

Profile::~Profile()
{
	
}

}
