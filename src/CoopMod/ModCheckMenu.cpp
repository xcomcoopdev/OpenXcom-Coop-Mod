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
#include <algorithm>
#include <functional>
#include "ServerList.h"
#include "../Engine/Logger.h"
#include "../Savegame/SavedGame.h"
#include "../Engine/Game.h"
#include "../Engine/Action.h"
#include "../Engine/Exception.h"
#include "../Engine/Options.h"
#include "../Engine/Unicode.h"
#include "../Mod/Mod.h"
#include "../Engine/LocalizedText.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Interface/Text.h"
#include "../Interface/TextList.h"
#include "../Interface/ToggleTextButton.h"
#include "../Interface/ArrowButton.h"

#include "ModCheckMenu.h"

namespace OpenXcom
{

struct compareModName
{
	bool _reverse;

	compareModName(bool reverse) : _reverse(reverse) {}

	bool operator()(const ModInfoCoop& a, const ModInfoCoop& b) const
	{
		if (a.reserved == b.reserved)
		{
			return Unicode::naturalCompare(a.modName, b.modName);
		}
		else
		{
			return _reverse ? b.reserved : a.reserved;
		}
	}
};

struct compareModRequired
{
	bool _reverse;

	compareModRequired(bool reverse) : _reverse(reverse) {}

	bool operator()(const ModInfoCoop& a, const ModInfoCoop& b) const
	{
		if (a.reserved == b.reserved)
		{
			return Unicode::naturalCompare(a.modRequired, b.modRequired);
		}
		else
		{
			return _reverse ? b.reserved : a.reserved;
		}
	}
};

/**
 * Initializes all the elements in the Saved Game screen.
 * @param game Pointer to the core game.
 * @param origin Game section that originated this state.
 * @param firstValidRow First row containing saves.
 * @param autoquick Show auto/quick saved games?
 */
ModCheckMenu::ModCheckMenu(std::string modHash) : _sortable(true), _modHash(modHash)
{
	_screen = false;

	bool isMobile = false;
#ifdef __MOBILE__
	isMobile = true;
#endif

	// Create objects
	_window = new Window(this, 320, 200, 0, 0, POPUP_BOTH);

	int y = 179; // 172
	int h = 16;
	int wCancel = 44;
	int x = 8;

	_btnCancel = new TextButton(wCancel, h, x, y);

	_txtTitle = new Text(310, 17, 5, 7);
	_txtModName = new Text(115, 9, 16, isMobile ? 40 : 32);
	_txtModRequired = new Text(50, 9, 154, isMobile ? 40 : 32);
	_lstMods = new TextList(298, isMobile ? 96 : 104, 8, isMobile ? 50 : 42);

	_sortModName = new ArrowButton(ARROW_NONE, 11, 8, 16, isMobile ? 40 : 32);
	_sortModRequired = new ArrowButton(ARROW_NONE, 11, 8, 154, isMobile ? 40 : 32);

	// Set palette
	setInterface("geoscape", true, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "saveMenus");
	add(_btnCancel, "button", "saveMenus");
	add(_txtTitle, "text", "saveMenus");
	add(_txtModName, "text", "saveMenus");
	add(_txtModRequired, "text", "saveMenus");
	add(_lstMods, "list", "saveMenus");
	add(_sortModName, "text", "saveMenus");
	add(_sortModRequired, "text", "saveMenus");

	// Set up objects
	setWindowBackground(_window, "saveMenus");

	Uint8 color = 239; // 239 or 255

	if (_game->getSavedGame())
	{
		if (_game->getSavedGame()->getSavedBattle())
		{
			color = 255;
		}
	}

	_btnCancel->setText(tr("STR_CANCEL"));
	_btnCancel->onMouseClick((ActionHandler)&ModCheckMenu::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&ModCheckMenu::btnCancelClick, Options::keyCancel);

	_txtTitle->setBig();
	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setText("INCOMPATIBLE MODS");

	_txtModName->setText("Mod Name");
	_txtModRequired->setText("Status");

	_lstMods->setColumns(2, 139, 159); 
	_lstMods->setSelectable(true);
	_lstMods->setBackground(_window);
	_lstMods->setMargin(8);

	_sortModName->setX(_sortModName->getX() + _txtModName->getTextWidth() + 5);
	_sortModName->onMouseClick((ActionHandler)&ModCheckMenu::sortModNameClick);

	_sortModRequired->setX(_sortModRequired->getX() + _txtModRequired->getTextWidth() + 5);
	_sortModRequired->onMouseClick((ActionHandler)&ModCheckMenu::sortModRequiredClick);

	updateArrows();

	updateModList();

}

/**
 *
 */
ModCheckMenu::~ModCheckMenu()
{

}

/**
 * Refreshes the saves list.
 */
void ModCheckMenu::init()
{
	State::init();

	updateModList();
	
	_origin = OPT_MENU;

	if (_game->getSavedGame())
	{
		if (_game->getSavedGame()->getSavedBattle())
		{
			_origin = OPT_BATTLESCAPE;
		}
	}

	if (_origin == OPT_BATTLESCAPE)
	{
		applyBattlescapeTheme("saveMenus");
	}

}

/**
 * Updates the sorting arrows based
 * on the current setting.
 */
void ModCheckMenu::updateArrows()
{
	_sortModName->setShape(ARROW_NONE);
	_sortModRequired->setShape(ARROW_NONE);
	switch (Options::modOrder)
	{
	case SORT_NAME_MOD_ASC:
		_sortModName->setShape(ARROW_SMALL_UP);
		break;
	case SORT_NAME_MOD_DESC:
		_sortModName->setShape(ARROW_SMALL_DOWN);
		break;
	case SORT_REQUIRED_MOD_ASC:
		_sortModRequired->setShape(ARROW_SMALL_UP);
		break;
	case SORT_REQUIRED_MOD_DESC:
		_sortModRequired->setShape(ARROW_SMALL_DOWN);
		break;
	}
}

void ModCheckMenu::updateModList()
{

	_mods.clear();

	// Local mods
	std::vector<std::string> local_mod_names = _game->getMod()->getCoopModList();

	if (_modHash != "")
	{
		// Required mods from the host/server
		std::vector<std::string> mods = _game->getCoopMod()->splitVectorMod(_modHash, ";");

		// Check if any required mod is missing locally
		for (auto& mod : mods)
		{
			if (mod == "")
				continue;

			bool found = std::find(
							 local_mod_names.begin(),
							 local_mod_names.end(),
							 mod) != local_mod_names.end();

			
			if (mod == "xcom1")
			{
				mod = "xcom1 (UFO Defense)";
			}

			if (mod == "xcom2")
			{
				mod = "xcom2 (Terror from the Deep)";
			}

			if (!found)
			{
				_mods.push_back(ModInfoCoop({mod, "Required mod not enabled."}));
			}
			else
			{
				_mods.push_back(ModInfoCoop({mod, "This mod is enabled."}));
			}
		}

		// Check if local player has any extra mods
		for (auto& local_mod : local_mod_names)
		{
			if (local_mod == "")
				continue;

			bool found = std::find(
							 mods.begin(),
							 mods.end(),
							 local_mod) != mods.end();

			if (local_mod == "xcom1")
			{
				local_mod = "xcom1 (UFO Defense)";
			}

			if (local_mod == "xcom2")
			{
				local_mod = "xcom2 (Terror from the Deep)";
			}

			if (!found)
			{
				_mods.push_back(ModInfoCoop({local_mod, "Disable extra mod."}));
			}
		}
	}

	_lstMods->clearList();
	sortList(Options::modOrder);

}

/**
 * Sorts the save game list.
 * @param sort Order to sort the games in.
 */
void ModCheckMenu::sortList(modSort sort)
{
	switch (sort)
	{
	case SORT_NAME_MOD_ASC:
		std::sort(_mods.begin(), _mods.end(), compareModName(false));
		break;
	case SORT_NAME_MOD_DESC:
		std::sort(_mods.rbegin(), _mods.rend(), compareModName(true));
		break;
	case SORT_REQUIRED_MOD_ASC:
		std::sort(_mods.begin(), _mods.end(), compareModRequired(false));
		break;
	case SORT_REQUIRED_MOD_DESC:
		std::sort(_mods.rbegin(), _mods.rend(), compareModRequired(true));
		break;
	}
	updateList();
}

/**
 * Updates the save game list with the current list
 * of available savegames.
 */
void ModCheckMenu::updateList()
{
	int row = 0;
	int color = _lstMods->getSecondaryColor();
	for (const auto& serverInfo : _mods)
	{

		std::string name = serverInfo.modName; 
		std::string required = serverInfo.modRequired;
		_lstMods->addRow(2, name.c_str(), required.c_str());
		if (serverInfo.reserved && _origin != OPT_BATTLESCAPE)
		{
			_lstMods->setRowColor(row, color);
		}
		row++;
	}
}

/**
 * Returns to the previous screen.
 * @param action Pointer to an action.
 */
void ModCheckMenu::btnCancelClick(Action*)
{
	_game->popState();
}

/**
 * Sorts the saves by name.
 * @param action Pointer to an action.
 */
void ModCheckMenu::sortModNameClick(Action*)
{
	if (_sortable)
	{
		if (Options::modOrder == SORT_NAME_MOD_ASC)
		{
			Options::modOrder = SORT_NAME_MOD_DESC;
		}
		else
		{
			Options::modOrder = SORT_NAME_MOD_ASC;
		}
		updateArrows();
		_lstMods->clearList();
		sortList(Options::modOrder);
	}
}

/**
 * Sorts the saves by date.
 * @param action Pointer to an action.
 */
void ModCheckMenu::sortModRequiredClick(Action*)
{
	if (_sortable)
	{
		if (Options::modOrder == SORT_REQUIRED_MOD_ASC)
		{
			Options::modOrder = SORT_REQUIRED_MOD_DESC;
		}
		else
		{
			Options::modOrder = SORT_REQUIRED_MOD_ASC;
		}
		updateArrows();
		_lstMods->clearList();
		sortList(Options::modOrder);
	}
}

void ModCheckMenu::disableSort()
{
	_sortable = false;
}

}
