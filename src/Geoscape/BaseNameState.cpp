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
#include "BaseNameState.h"
#include "../Engine/Game.h"
#include "../Engine/Action.h"
#include "../Mod/Mod.h"
#include "../Interface/Window.h"
#include "../Interface/Text.h"
#include "../Interface/TextEdit.h"
#include "../Interface/TextButton.h"
#include "../Savegame/Base.h"
#include "../Basescape/PlaceLiftState.h"
#include "../Engine/Options.h"
#include "../Engine/RNG.h"
#include "../CoopMod/CoopState.h"
#include "../CoopMod/connectionTCP.h"

namespace OpenXcom
{

/**
 * Initializes all the elements in a Base Name window.
 * @param game Pointer to the core game.
 * @param base Pointer to the base to name.
 * @param globe Pointer to the Geoscape globe.
 * @param first Is this the first base in the game?
 * @param fixedLocation Is this the first base in the game on a fixed location?
 */
BaseNameState::BaseNameState(Base *base, Globe *globe, bool first, bool fixedLocation) : _base(base), _globe(globe), _first(first), _fixedLocation(fixedLocation)
{
	_globe->onMouseOver(0);

	_screen = false;

	// Create objects
	_window = new Window(this, 192, 80, 32, 60, POPUP_BOTH);
	_btnOk = new TextButton(162, 12, 47, 118);
	_txtTitle = new Text(182, 17, 37, 70);
	_edtName = new TextEdit(this, 127, 16, 59, 94);

	// Set palette
	setInterface("baseNaming");

	add(_window, "window", "baseNaming");
	add(_btnOk, "button", "baseNaming");
	add(_txtTitle, "text", "baseNaming");
	add(_edtName, "text", "baseNaming");

	centerAllSurfaces();

	// Set up objects
	setWindowBackground(_window, "baseNaming");

	_btnOk->setText(tr("STR_OK"));
	_btnOk->onMouseClick((ActionHandler)&BaseNameState::btnOkClick);
	//_btnOk->onKeyboardPress((ActionHandler)&BaseNameState::btnOkClick, Options::keyOk);
	_btnOk->onKeyboardPress((ActionHandler)&BaseNameState::btnOkClick, Options::keyCancel);

	//something must be in the name before it is acceptable
	_btnOk->setVisible(false);

	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setBig();
	_txtTitle->setText(tr("STR_BASE_NAME"));

	if (!_game->getMod()->getBaseNamesFirst().empty())
	{
		std::ostringstream ss;
		int pickFirst = RNG::seedless(0, _game->getMod()->getBaseNamesFirst().size() - 1);
		ss << _game->getMod()->getBaseNamesFirst().at(pickFirst);
		if (!_game->getMod()->getBaseNamesMiddle().empty())
		{
			int pickMiddle = RNG::seedless(0, _game->getMod()->getBaseNamesMiddle().size() - 1);
			ss << " " << _game->getMod()->getBaseNamesMiddle().at(pickMiddle);
		}
		if (!_game->getMod()->getBaseNamesLast().empty())
		{
			int pickLast = RNG::seedless(0, _game->getMod()->getBaseNamesLast().size() - 1);
			ss << " " << _game->getMod()->getBaseNamesLast().at(pickLast);
		}
		_edtName->setText(ss.str());
		_btnOk->setVisible(true);
	}

	_edtName->setBig();
	_edtName->setFocus(true, false);
	_edtName->onChange((ActionHandler)&BaseNameState::edtNameChange);
}

/**
 *
 */
BaseNameState::~BaseNameState()
{

}

/**
 * Updates the base name and disables the OK button
 * if no name is entered.
 * @param action Pointer to an action.
 */
void BaseNameState::edtNameChange(Action *action)
{
	if (action->getDetails()->key.keysym.sym == SDLK_RETURN ||
		action->getDetails()->key.keysym.sym == SDLK_KP_ENTER)
	{
		if (!_edtName->getText().empty())
		{
			btnOkClick(action);
		}
	}
	else
	{
		_btnOk->setVisible(!_edtName->getText().empty());
	}
}

/**
 * Sets the base name and confirms (test automation).
 */
void BaseNameState::setNameAndConfirm(const std::string &name)
{
	_edtName->setText(name);
	btnOkClick(nullptr);
}

/**
 * Returns to the previous screen
 * @param action Pointer to an action.
 */
void BaseNameState::btnOkClick(Action *)
{
	if (!_edtName->getText().empty())
	{
		_base->setName(_edtName->getText());
		_game->popState(); // pop BaseNameState

		if (!_fixedLocation)
		{
			_game->popState(); // pop ConfirmNewBaseState or BuildNewBaseState
			if (!_first)
			{
				_game->popState(); // pop BuildNewBaseState
			}
		}

		if (!_first || Options::customInitialBase)
		{
			_game->pushState(new PlaceLiftState(_base, _globe, _first));
		}

		// coop
		if (_game->getCoopMod()->getCoopStatic() == true && _first && _game->getCoopMod()->getServerOwner() == false)
		{

			Json::Value root;
			root["state"] = "close_load_progress";

			_game->getCoopMod()->sendTCPPacketData(root.toStyledString());

			// campaign flow: ship the freshly built world to the host so its
			// "all players placed bases" gate can open (F2); on a resume or
			// rejoin this also reports us loaded (F3/F4; stray acks with no
			// waiting dialog are ignored). Then hold with frozen time until
			// the host resumes the campaign (D5).
			if (connectionTCP::session.lobbyMode != 0)
			{
				_game->getCoopMod()->pushProgressToHostSilently();

				Json::Value ack;
				ack["state"] = "resume_ack";
				_game->getCoopMod()->sendTCPPacketData(ack.toStyledString());

				_game->pushState(new CoopState(COOP_DLG_CLIENT_HOLD));
			}

		}

		// new-campaign flow: the host holds here until every player's world
		// blob has arrived (F2)
		if (_game->getCoopMod()->getCoopStatic() == true && _first && _game->getCoopMod()->getServerOwner() == true && connectionTCP::session.lobbyMode == 1)
		{
			// PRD-J02: in JOINT the host neither waits for client base blobs nor
			// pushes a dialog here. A dialog would block the geoscape init that
			// finalizes the world (month advance + start-of-game maintenance);
			// GeoscapeState::init streams the SETTLED world and holds in
			// COOP_DLG_RESUME_ACK_WAIT there.
			if (!_game->getCoopMod()->isJointCampaign())
			{
				_game->pushState(new CoopState(COOP_DLG_WAIT_BASES));
			}
		}

	}
}

}
