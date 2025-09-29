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
#include <sstream>
#include "SackSoldierState.h"
#include "../Engine/Game.h"
#include "../Mod/Mod.h"
#include "../Engine/Options.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Interface/Text.h"
#include "../Savegame/Base.h"
#include "../Savegame/ItemContainer.h"
#include "../Savegame/Soldier.h"
#include "../Mod/Armor.h"

#include "../Menu/LoadGameState.h"
#include "../Menu/SaveGameState.h"

namespace OpenXcom
{

/**
 * Initializes all the elements in a Sack Soldier window.
 * @param game Pointer to the core game.
 * @param base Pointer to the base to get info from.
 * @param soldierId ID of the soldier to sack.
 */
SackSoldierState::SackSoldierState(Base *base, size_t soldierId) : _base(base), _soldierId(soldierId)
{
	_screen = false;

	// Create objects
	_window = new Window(this, 152, 80, 84, 60);
	_btnOk = new TextButton(44, 16, 100, 115);
	_btnCancel = new TextButton(44, 16, 176, 115);
	_txtTitle = new Text(142, 9, 89, 75);
	_txtSoldier = new Text(142, 9, 89, 85);

	// Set palette
	setInterface("sackSoldier");

	add(_window, "window", "sackSoldier");
	add(_btnOk, "button", "sackSoldier");
	add(_btnCancel, "button", "sackSoldier");
	add(_txtTitle, "text", "sackSoldier");
	add(_txtSoldier, "text", "sackSoldier");

	centerAllSurfaces();

	// Set up objects
	setWindowBackground(_window, "sackSoldier");

	_btnOk->setText(tr("STR_OK"));
	_btnOk->onMouseClick((ActionHandler)&SackSoldierState::btnOkClick);
	_btnOk->onKeyboardPress((ActionHandler)&SackSoldierState::btnOkClick, Options::keyOk);

	_btnCancel->setText(tr("STR_CANCEL_UC"));
	_btnCancel->onMouseClick((ActionHandler)&SackSoldierState::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&SackSoldierState::btnCancelClick, Options::keyCancel);

	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setText(tr("STR_SACK"));

	// COOP
	if (_game->getCoopMod()->getCoopStatic() == true && _base->_coopBase == true)
	{
		_txtTitle->setText("MOVE");
	}

	std::ostringstream ss;
	ss << _base->getSoldiers()->at(_soldierId)->getName(true) << "?";

	_txtSoldier->setAlign(ALIGN_CENTER);
	_txtSoldier->setText(ss.str());
}

/**
 *
 */
SackSoldierState::~SackSoldierState()
{

}

/**
 * Sacks the soldier and returns
 * to the previous screen.
 * @param action Pointer to an action.
 */
void SackSoldierState::btnOkClick(Action *)
{

	Soldier* soldier = _base->getSoldiers()->at(_soldierId);

	// coop campaign
	if (_game->getCoopMod()->getCoopStatic() == true && _base->_coopBase == true && _game->getCoopMod()->playerInsideCoopBase == true && _game->getCoopMod()->getCoopCampaign() == true)
	{

		// tallentetaan toisen pelaaja base (vain CLIENT) esim. sotilaat jne
		std::string filename = "";
		std::string filepath = Options::getMasterUserFolder() + filename;

		if (_game->getCoopMod()->getServerOwner() == true)
		{

			filename = "host/basehost.data";
		}
		else
		{

			filename = "client/basehost.data";
		}

		if (OpenXcom::CrossPlatform::fileExists(filepath))
		{

			SavedGame* basehost_save = new SavedGame();

			basehost_save->load(filename, _game->getMod(), _game->getLanguage());

			// if save found
			if (basehost_save)
			{

				for (auto& saved_base : *basehost_save->getBases())
				{
					auto& soldiers = *saved_base->getSoldiers(); // Reference to the vector of soldiers

					for (auto it = soldiers.begin(); it != soldiers.end(); /* no ++it here */)
					{
						// Check if the soldier belongs to this coop base and has the matching ID
						if ((*it)->getCoopBase() == _base->getId() && (*it)->getId() == soldier->getId())
						{
							delete *it;              // Free memory
							it = soldiers.erase(it); // Remove pointer from vector and update iterator
						}
						else
						{
							++it; // Only move to next if not erased
						}
					}
				}


				// Lisätään uudet sotilaat ensimmäisen tukikohdan sotilaslistaan
				auto& target_soldiers = *basehost_save->getBases()->front()->getSoldiers();

				soldier->setCoopCraft(-1);
				soldier->setCoopCraftType("");

				soldier->setCoopBase(-1);

				soldier->setCraftAndMoveEquipment(0, _base, _game->getSavedGame()->getMonthsPassed() == -1);

				target_soldiers.push_back(soldier);

				// save changes
				basehost_save->save(filename, _game->getMod());

			}
		}
	}

	if (soldier->getArmor()->getStoreItem())
	{
		_base->getStorageItems()->addItem(soldier->getArmor()->getStoreItem());
	}
	_base->getSoldiers()->erase(_base->getSoldiers()->begin() + _soldierId);
	delete soldier;

	_game->popState();

}

/**
 * Returns to the previous screen.
 * @param action Pointer to an action.
 */
void SackSoldierState::btnCancelClick(Action *)
{
	_game->popState();
}

}
