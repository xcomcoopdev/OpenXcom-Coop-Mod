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
#include "TransferNoticeState.h"

#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Geoscape/GeoscapeState.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleInterface.h"

namespace OpenXcom
{

TransferNoticeState::TransferNoticeState(const std::string &message)
{
	_screen = false;

	_window = new Window(this, 256, 88, 32, 56, POPUP_BOTH);
	_txtMessage = new Text(236, 40, 42, 68);
	_btnOk = new TextButton(120, 16, 100, 118);

	// Adopt the palette of whatever screen we're over - no palette swap, no
	// flicker, works on geoscape, basescape and the peer-base view alike.
	// Color indices come from an interface designed for that palette, or the
	// text is illegible (sackSoldier's dark-blue text over PAL_GEOSCAPE).
	// Skip other notices when deciding the context: with two notices stacked,
	// the "top state" is the first notice, not the screen underneath.
	std::string category = "sackSoldier";
	std::string textElement = "text";
	State* context = nullptr;
	for (auto it = _game->getStates().rbegin(); it != _game->getStates().rend(); ++it)
	{
		if (dynamic_cast<TransferNoticeState*>(*it) == nullptr)
		{
			context = *it;
			break;
		}
	}
	if (context)
	{
		setStatePalette(context->getPalette());
		if (dynamic_cast<GeoscapeState*>(context))
		{
			category = "geoManufactureComplete"; // standard geoscape popup colors
			textElement = "text1";
		}
	}
	_category = category;

	add(_window, "window", category);
	add(_txtMessage, textElement, category);
	add(_btnOk, "button", category);

	centerAllSurfaces();
	setWindowBackground(_window, category);

	_txtMessage->setAlign(ALIGN_CENTER);
	_txtMessage->setWordWrap(true);
	_txtMessage->setText(message);

	_btnOk->setText(tr("STR_OK"));
	_btnOk->onMouseClick((ActionHandler)&TransferNoticeState::btnOkClick);
	_btnOk->onKeyboardPress((ActionHandler)&TransferNoticeState::btnOkClick, Options::keyOk);
	_btnOk->onKeyboardPress((ActionHandler)&TransferNoticeState::btnOkClick, Options::keyCancel);
}

void TransferNoticeState::btnOkClick(Action *)
{
	_game->popState();
}

}
