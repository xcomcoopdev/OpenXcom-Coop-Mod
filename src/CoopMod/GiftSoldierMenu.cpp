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
#include "GiftSoldierMenu.h"

#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Soldier.h"
#include "connectionTCP.h"

namespace OpenXcom
{

int GiftSoldierMenu::resolveOwnerId(Soldier *soldier)
{
	if (soldier->getOwnerPlayerId() != 999)
	{
		return soldier->getOwnerPlayerId();
	}
	// 999 = never explicitly assigned. Such a soldier belongs to the player
	// whose save it lives in - i.e. the LOCAL player, on whichever machine
	// the dialog is open. (The old fallback returned 0/host, which on the
	// client's machine offered the client their own name as a transfer
	// target for their own fresh soldiers.)
	return connectionTCP::localSeat();
}

GiftSoldierMenu::GiftSoldierMenu(Soldier *soldier, int currentOwnerId) : _soldier(soldier)
{
	_screen = false;

	// One button per player that is not the current owner. Player ids follow
	// the co-op convention: 0 = host, 1 = client. Built as a list so more
	// players slot in when the game grows past two.
	//
	// Name getters are machine-relative, not role-fixed: getHostName() is the
	// LOCAL player's own name (every machine writes its own name box there)
	// and getCurrentClientName() is the PEER's name (from received packets).
	connectionTCP *coop = _game->getCoopMod();
	int localPlayerId = connectionTCP::localSeat();

	std::vector<std::pair<int, std::string> > targets;
	for (int playerId = 0; playerId <= 1; ++playerId)
	{
		if (playerId != currentOwnerId)
		{
			std::string name = (playerId == localPlayerId) ? coop->getHostName() : coop->getCurrentClientName();
			targets.push_back(std::make_pair(playerId, name));
		}
	}

	const int btnHeight = 16;
	const int btnSpacing = 4;
	const int windowWidth = 240;
	const int windowHeight = 60 + (int)(targets.size() + 1) * (btnHeight + btnSpacing);
	const int windowX = (320 - windowWidth) / 2;
	const int windowY = (200 - windowHeight) / 2;

	_window = new Window(this, windowWidth, windowHeight, windowX, windowY, POPUP_BOTH);
	_txtTitle = new Text(windowWidth - 20, 32, windowX + 10, windowY + 12);

	int y = windowY + 48;
	for (size_t i = 0; i < targets.size(); ++i)
	{
		_btnTargets.push_back(new TextButton(windowWidth - 40, btnHeight, windowX + 20, y));
		_targetIds.push_back(targets[i].first);
		y += btnHeight + btnSpacing;
	}
	_btnCancel = new TextButton(windowWidth - 40, btnHeight, windowX + 20, y);

	// sackSoldier: a base-palette dialog interface. Using pauseMenu here
	// (geoscape palette) forced a hardware palette swap when opened over the
	// basescape soldier screens, flashing the whole screen on open and close.
	// The battle-game param switches to the battlescape palette in battle.
	// Out of battle, sackSoldier's base palette avoids a hardware palette flash
	// over the basescape soldier screens. In the battlescape those colors are
	// illegible, so match the coop lobby window exactly (geoscape interface with
	// alterPal, saveMenus element colors, under the battle palette).
	SavedBattleGame *battle = _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0;
	std::string cat = "sackSoldier";
	if (battle)
	{
		setInterface("geoscape", true, battle);
		cat = "saveMenus";
	}
	else
	{
		setInterface("sackSoldier", false, 0);
	}

	add(_window, "window", cat);
	add(_txtTitle, "text", cat);
	for (auto *btn : _btnTargets)
	{
		add(btn, "button", cat);
	}
	add(_btnCancel, "button", cat);

	centerAllSurfaces();
	if (battle)
	{
		// Same as the coop lobby in battle: uniform battlescape theme color +
		// high contrast + TAC00 background.
		setWindowBackground(_window, cat);
		applyBattlescapeTheme(cat);
	}
	else
	{
		setWindowBackground(_window, cat);
	}

	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setWordWrap(true);
	_txtTitle->setText("Gift " + _soldier->getName() + " to another player?");

	for (size_t i = 0; i < _btnTargets.size(); ++i)
	{
		std::string name = targets[i].second;
		if (name.empty())
		{
			name = targets[i].first == 0 ? "HOST" : "CLIENT";
		}
		_btnTargets[i]->setText(name);
		_btnTargets[i]->onMouseClick((ActionHandler)&GiftSoldierMenu::btnGiftClick);
	}

	_btnCancel->setText(tr("STR_CANCEL"));
	_btnCancel->onMouseClick((ActionHandler)&GiftSoldierMenu::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&GiftSoldierMenu::btnCancelClick, Options::keyCancel);
}

void GiftSoldierMenu::btnGiftClick(Action *action)
{
	for (size_t i = 0; i < _btnTargets.size(); ++i)
	{
		if (action->getSender() == _btnTargets[i])
		{
			_game->getCoopMod()->giftSoldier(_soldier, _targetIds[i], true);
			break;
		}
	}
	_game->popState();
}

void GiftSoldierMenu::btnCancelClick(Action *)
{
	_game->popState();
}

}
