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
#include "../Basescape/SoldierArmorState.h"
#include "../Basescape/SoldierAvatarState.h"
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
#include "../Interface/BattlescapeButton.h"
#include "../Interface/Text.h"
#include "../Interface/TextEdit.h"
#include "../Mod/Armor.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleInterface.h"
#include "../Mod/RuleInventory.h"
#include "../Mod/RuleItem.h"
#include "../Mod/RuleSoldier.h"
#include "../Savegame/Base.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/Craft.h"
#include "../Savegame/ItemContainer.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/Soldier.h"
#include "../Savegame/Tile.h"
#include "../Ufopaedia/Ufopaedia.h"
#include "../Battlescape/BattlescapeGenerator.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/ExtendedInventoryLinksState.h"
#include "../Battlescape/Inventory.h"
#include "../Battlescape/InventoryLoadState.h"
#include "../Battlescape/InventoryPersonalState.h"
#include "../Battlescape/InventorySaveState.h"

#include "../Battlescape/TileEngine.h"
#include "../Battlescape/UnitInfoState.h"
#include <algorithm>

#include "Profile.h"
#include "../Interface/Window.h"

#include "CoopState.h"

namespace OpenXcom
{

static const int _templateBtnX = 288;
static const int _createTemplateBtnY = 90;
static const int _applyTemplateBtnY = 113;

/**
 * Initializes all the elements in the Inventory screen.
 * @param game Pointer to the core game.
 * @param tu Does Inventory use up Time Units?
 * @param parent Pointer to parent Battlescape.
 */
Profile::Profile(bool clientInBattle, Base *base, bool inBattle) : _clientInBattle(clientInBattle), _inBattle(inBattle), _base(base),
																							  _resetCustomDeploymentBackup(false), _reloadUnit(false), _globalLayoutIndex(-1)
{

	int x = 50;

	// Create objects
	 _bg = new Surface(320, 200, 0, 0);
	_soldier = new Surface(320, 200, 0, 0);
	 _txtName = new Text(210, 17, 30+18, 130);
	

	_btnMessage = new TextButton(180, 18, x + 18, 152);

	_btnMessage->setText("OK");
	std::string playerName = _game->getCoopMod()->getCurrentClientName();

	std::string result = "ERROR";

	if (_game->getCoopMod()->getHost() == true)
	{
		result = playerName + " has joined the game";
	}
	else
	{
		result = "You have joined the " + playerName + "'s game";
	}

	_txtName->setText(result);

	_txtName->setAlign(ALIGN_CENTER);
	_txtName->setBig();
	_txtName->setHighContrast(true);

	_btnCreateTemplate = new BattlescapeButton(64, 64, x + 70, 50);
	
	_txtTus = new Text(40, 9, 245, 24);
	_txtWeight = new Text(70, 9, 245, 24);
	_txtStatLine1 = new Text(70, 9, 245, 32);
	_txtStatLine2 = new Text(70, 9, 245, 40);
	_txtStatLine3 = new Text(70, 9, 245, 48);
	_txtStatLine4 = new Text(70, 9, 245, 56);
	_txtItem = new Text(160, 9, 128, 140);
	_txtAmmo = new Text(66, 24, 254, 64);
	_btnOk = new BattlescapeButton(35, 22, 237, 1);
	_btnPrev = new BattlescapeButton(23, 22, 273, 1);
	_btnNext = new BattlescapeButton(23, 22, 297, 1);
	_btnUnload = new BattlescapeButton(32, 25, 288, 32);
	_btnGround = new BattlescapeButton(32, 15, 289, 137);
	_btnRank = new BattlescapeButton(26, 23, 0, 0);
	_btnArmor = new BattlescapeButton(RuleInventory::PAPERDOLL_W, RuleInventory::PAPERDOLL_H, RuleInventory::PAPERDOLL_X, RuleInventory::PAPERDOLL_Y);

	_btnApplyTemplate = new BattlescapeButton(32, 32, _templateBtnX, _applyTemplateBtnY);


	// Set palette
	setStandardPalette("PAL_BATTLESCAPE");

	add(_bg);

	add(_txtName, "textName", "inventory", _bg);
	
	add(_btnMessage, "buttonOK", "inventory", _bg);
	add(_btnOk, "buttonOK", "inventory", _bg);
	add(_btnCreateTemplate, "buttonCreate", "inventory", _bg);


	// move the TU display down to make room for the weight display
	if (Options::showMoreStatsInInventoryView)
	{
		_txtTus->setY(_txtTus->getY() + 8);
	}

	centerAllSurfaces();


	_btnMessage->onMouseClick((ActionHandler)&Profile::buttonOK);

	// laod profile image
	Surface *a = new Surface();
	a->loadImage("multiplayer/avatar.png");
	a->blitNShade(_btnCreateTemplate, 0, 0);
	_btnCreateTemplate->initSurfaces();

}

void Profile::buttonOK(Action *)
{

	_game->popState();

	// if the client is in battle and the host is not, send the host a file and a notification
	if (_clientInBattle == true && _inBattle == false)
	{

		// client only!
		if (_game->getCoopMod()->getHost() == false)
		{

			Json::Value root;

			root["state"] = "SEND_FILE_HOST_SAVE";

			_game->getCoopMod()->sendTCPPacketData(root.toStyledString());

		}

	}
	// CHECK IF THE HOST IS IN BATTLE — IF SO, ADD JOINERS; OTHERWISE DO NOTHING
	else if (_inBattle == true)
	{

		// only client!
		if (_game->getCoopMod()->getHost() == false)
		{

			Json::Value root;

			root["state"] = "SEND_FILE_CLIENT_SAVE";

			_game->getCoopMod()->inventory_battle_window = false;

			_game->getCoopMod()->sendTCPPacketData(root.toStyledString());

			_game->pushState(new CoopState(1));

		}
		


	}

}

static void _clearInventoryTemplate(std::vector<EquipmentLayoutItem *> &inventoryTemplate)
{
	Collections::deleteAll(inventoryTemplate);
}

/**
 *
 */
Profile::~Profile()
{
	
}

void Profile::setGlobalLayoutIndex(int index, bool armorChanged)
{
	/*
	_globalLayoutIndex = index;
	if (armorChanged)
	{
		_reloadUnit = true;
	}
	*/
}

/**
 * Updates all soldier stats when the soldier changes.
 */
void Profile::init()
{
	
	State::init();
	
}



void Profile::updateTemplateButtons(bool isVisible)
{
	if (isVisible)
	{
		if (_curInventoryTemplate.empty())
		{
			Surface *a = new Surface();
			//a->loadImage("UFOINTRO/invcopy.png");
			a->loadImage("multiplayer/avatar.png");
			a->blitNShade(_btnCreateTemplate, 0, 0);

			// use "empty template" icons
			//_game->getMod()->getSurface("InvCopy")->blitNShade(_btnCreateTemplate, 0, 0);
			//_game->getMod()->getSurface("InvPasteEmpty")->blitNShade(_btnApplyTemplate, 0, 0);
			//_btnApplyTemplate->setTooltip("STR_CLEAR_INVENTORY");
		}
		else
		{
			// use "active template" icons
			//_game->getMod()->getSurface("InvCopyActive")->blitNShade(_btnCreateTemplate, 0, 0);
			//_game->getMod()->getSurface("InvPaste")->blitNShade(_btnApplyTemplate, 0, 0);
			//_btnApplyTemplate->setTooltip("STR_APPLY_INVENTORY_TEMPLATE");
		}
		_btnCreateTemplate->initSurfaces();
		//_btnApplyTemplate->initSurfaces();
	}
	else
	{
		_btnCreateTemplate->clear();
		//_btnApplyTemplate->clear();
	}
}




}
