﻿/*
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
#include "CraftsState.h"
#include <sstream>
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Mod/Mod.h"
#include "../Engine/LocalizedText.h"
#include "../Engine/Options.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Interface/Text.h"
#include "../Interface/TextList.h"
#include "../Savegame/Craft.h"
#include "../Mod/RuleCraft.h"
#include "../Savegame/Base.h"
#include "../Menu/ErrorMessageState.h"
#include "CraftInfoState.h"
#include "SellState.h"
#include "../Savegame/SavedGame.h"
#include "../Mod/RuleInterface.h"
#include "../Ufopaedia/Ufopaedia.h"

namespace OpenXcom
{

/**
 * Initializes all the elements in the Equip Craft screen.
 * @param game Pointer to the core game.
 * @param base Pointer to the base to get info from.
 */
CraftsState::CraftsState(Base *base) : _base(base)
{
	// Create objects
	_window = new Window(this, 320, 200, 0, 0);
	_btnOk = new TextButton(288, 16, 16, 176);
	_txtTitle = new Text(298, 17, 16, 8);
	_txtBase = new Text(298, 17, 16, 24);
	_txtName = new Text(94, 9, 16, 40);
	_txtStatus = new Text(50, 9, 110, 40);
	_txtWeapon = new Text(50, 17, 160, 40);
	_txtCrew = new Text(58, 9, 210, 40);
	_txtHwp = new Text(46, 9, 268, 40);
	_lstCrafts = new TextList(288, 112, 8, 58);

	// Set palette
	setInterface("craftSelect");

	add(_window, "window", "craftSelect");
	add(_btnOk, "button", "craftSelect");
	add(_txtTitle, "text", "craftSelect");
	add(_txtBase, "text", "craftSelect");
	add(_txtName, "text", "craftSelect");
	add(_txtStatus, "text", "craftSelect");
	add(_txtWeapon, "text", "craftSelect");
	add(_txtCrew, "text", "craftSelect");
	add(_txtHwp, "text", "craftSelect");
	add(_lstCrafts, "list", "craftSelect");

	centerAllSurfaces();

	// Set up objects
	setWindowBackground(_window, "craftSelect");

	_btnOk->setText(tr("STR_OK"));
	_btnOk->onMouseClick((ActionHandler)&CraftsState::btnOkClick);
	_btnOk->onKeyboardPress((ActionHandler)&CraftsState::btnOkClick, Options::keyCancel);

	_txtTitle->setBig();
	_txtTitle->setText(tr("STR_INTERCEPTION_CRAFT"));

	_txtBase->setBig();
	_txtBase->setText(tr("STR_BASE_").arg(_base->getName()));

	_txtName->setText(tr("STR_NAME_UC"));

	_txtStatus->setText(tr("STR_STATUS"));

	_txtWeapon->setText(tr("STR_WEAPON_SYSTEMS"));
	_txtWeapon->setWordWrap(true);

	_txtCrew->setText(tr("STR_CREW"));

	_txtHwp->setText(tr("STR_HWPS"));
	_lstCrafts->setColumns(5, 94, 68, 44, 46, 28);
	_lstCrafts->setSelectable(true);
	_lstCrafts->setBackground(_window);
	_lstCrafts->setMargin(8);
	_lstCrafts->onMouseClick((ActionHandler)&CraftsState::lstCraftsClick);
	_lstCrafts->onMouseClick((ActionHandler)&CraftsState::lstCraftsClick, SDL_BUTTON_RIGHT);
	_lstCrafts->onMouseClick((ActionHandler)&CraftsState::lstCraftsClick, SDL_BUTTON_MIDDLE);
}

/**
 *
 */
CraftsState::~CraftsState()
{

}

/**
 * The soldier names can change
 * after going into other screens.
 */
void CraftsState::init()
{
	State::init();

	initList(0);
}

/**
 * Shows the crafts in a list at specified offset/scroll.
 */
void CraftsState::initList(size_t scrl)
{
	_lstCrafts->clearList();
	for (const auto* craft : *_base->getCrafts())
	{
		std::ostringstream ss, ss2, ss3;
		ss << craft->getNumWeapons() << "/" << craft->getRules()->getWeapons();
		ss2 << craft->getNumTotalSoldiers();
		ss3 << craft->getNumTotalVehicles();
		_lstCrafts->addRow(5, craft->getName(_game->getLanguage()).c_str(), tr(craft->getStatus()).c_str(), ss.str().c_str(), ss2.str().c_str(), ss3.str().c_str());
	}

	if (scrl)
		_lstCrafts->scrollTo(scrl);
}

/**
 * Returns to the previous screen.
 * @param action Pointer to an action.
 */
void CraftsState::btnOkClick(Action *)
{

	_game->popState();

	if (_game->getSavedGame()->getMonthsPassed() > -1 && Options::storageLimitsEnforced && _base->storesOverfull())
	{
		_game->pushState(new SellState(_base, 0));
		_game->pushState(new ErrorMessageState(tr("STR_STORAGE_EXCEEDED").arg(_base->getName()), _palette, _game->getMod()->getInterface("craftSelect")->getElement("errorMessage")->color, "BACK01.SCR", _game->getMod()->getInterface("craftSelect")->getElement("errorPalette")->color));
	}
}

/**
 * Shows the selected craft's info.
 * @param action Pointer to an action.
 */
void CraftsState::lstCraftsClick(Action *action)
{
	auto& crafts = *_base->getCrafts();
	auto row = _lstCrafts->getSelectedRow();

	if (_game->isLeftClick(action))
	{
		if (crafts[row]->getStatus() != "STR_OUT")
		{
			_game->pushState(new CraftInfoState(_base, row));
		}
	}
	else if (_game->isRightClick(action))
	{
		bool shift = _game->isShiftPressed();
		if (shift && row < (crafts.size() - 1))
		{
			// move craft down in the list
			std::swap(crafts[row], crafts[row + 1]);

			// warp mouse
			if (row != _lstCrafts->getVisibleRows() - 1 + _lstCrafts->getScroll())
			{
				SDL_WarpMouse(action->getLeftBlackBand() + action->getXMouse(), action->getTopBlackBand() + action->getYMouse() + static_cast<Uint16>(8 * action->getYScale()));
			}
			else
			{
				_lstCrafts->scrollDown(false);
			}

			// reload the UI
			initList(_lstCrafts->getScroll());
		}
		if (!shift && row > 0)
		{
			// move craft up in the list
			std::swap(crafts[row], crafts[row - 1]);

			// warp mouse
			if (row != _lstCrafts->getScroll())
			{
				SDL_WarpMouse(action->getLeftBlackBand() + action->getXMouse(), action->getTopBlackBand() + action->getYMouse() - static_cast<Uint16>(8 * action->getYScale()));
			}
			else
			{
				_lstCrafts->scrollUp(false);
			}

			// reload the UI
			initList(_lstCrafts->getScroll());
		}
	}
	else if (_game->isMiddleClick(action))
	{
		std::string articleId = crafts[row]->getRules()->getType();
		Ufopaedia::openArticle(_game, articleId);
	}
}

}
