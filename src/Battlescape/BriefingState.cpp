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
#include "BriefingState.h"
#include "BattlescapeState.h"
#include "BattlescapeGame.h"
#include "AliensCrashState.h"
#include "../Engine/Game.h"
#include "../Engine/LocalizedText.h"
#include "../Interface/TextButton.h"
#include "../Interface/Text.h"
#include "../Interface/Window.h"
#include "InventoryState.h"
#include "NextTurnState.h"
#include "../Mod/Mod.h"
#include "../Savegame/Base.h"
#include "../Savegame/Craft.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/Ufo.h"
#include "../Mod/AlienDeployment.h"
#include "../Mod/RuleUfo.h"
#include "../Engine/Options.h"
#include "../Engine/RNG.h"
#include "../Engine/Screen.h"
#include "../Menu/CutsceneState.h"
#include "../Savegame/AlienMission.h"
#include "../Mod/RuleAlienMission.h"
#include "../Menu/SaveGameState.h"

// TEST
#include "../CoopMod/CoopMenu.h"
#include "../CoopMod/Profile.h"
#include "../Savegame/Waypoint.h"
#include "../Mod/RuleInventory.h"

namespace OpenXcom
{

bool isReadablePointer(void* ptr)
{
	if (!ptr)
		return false;

	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(ptr, &mbi, sizeof(mbi)))
	{
		DWORD protect = mbi.Protect;
		bool readable =
			(protect & PAGE_READONLY) ||
			(protect & PAGE_READWRITE) ||
			(protect & PAGE_EXECUTE_READ) ||
			(protect & PAGE_EXECUTE_READWRITE);

		return readable && mbi.State == MEM_COMMIT;
	}
	return false;
}

/**
 * Initializes all the elements in the Briefing screen.
 * @param game Pointer to the core game.
 * @param craft Pointer to the craft in the mission.
 * @param base Pointer to the base in the mission.
 * @param infoOnly Only show static info, when briefing is re-opened during the battle.
 * @param customBriefing Pointer to a custom briefing (used for Reinforcements notification).
 */
BriefingState::BriefingState(Craft *craft, Base *base, bool infoOnly, BriefingData *customBriefing) : _infoOnly(infoOnly), _disableCutsceneAndMusic(false)
{

	Options::baseXResolution = Options::baseXGeoscape;
	Options::baseYResolution = Options::baseYGeoscape;
	_game->getScreen()->resetDisplay(false);

	_screen = true;
	// Create objects
	_window = new Window(this, 320, 200, 0, 0);
	_btnOk = new TextButton(120, 18, 100, 164);
	_txtTitle = new Text(300, 32, 16, 24);
	_txtTarget = new Text(300, 17, 16, 40);
	_txtCraft = new Text(300, 17, 16, 56);
	_txtBriefing = new Text(274, 94, 16, 72);

	auto* battleSave = _game->getSavedGame()->getSavedBattle();

	std::string mission = battleSave->getMissionType();
	AlienDeployment *deployment = _game->getMod()->getDeployment(mission);
	if (mission == "STR_BASE_DEFENSE")
	{
		AlienDeployment* customDeployment = _game->getMod()->getDeployment(battleSave->getAlienCustomDeploy());
		if (customDeployment && !customDeployment->getBriefingData().desc.empty())
		{
			deployment = customDeployment;
		}
	}
	else
	{
		Ufo* ufo = 0;
		if (!deployment && craft)
		{
			ufo = dynamic_cast <Ufo*> (craft->getDestination());
			if (ufo) // landing site or crash site.
			{
				std::string ufoMissionName = ufo->getRules()->getType();
				if (!battleSave->getAlienCustomMission().empty())
				{
					// fake underwater UFO
					ufoMissionName = battleSave->getAlienCustomMission();
				}
				deployment = _game->getMod()->getDeployment(ufoMissionName);
			}
		}
	}

	std::string title = mission;
	std::string desc = title + "_BRIEFING";
	if (!deployment && !customBriefing) // none defined - should never happen, but better safe than sorry i guess.
	{
		setStandardPalette("PAL_GEOSCAPE", 0);
		_musicId = "GMDEFEND";
		_window->setBackground(_game->getMod()->getSurface("BACK16.SCR"));
	}
	else
	{
		BriefingData data = customBriefing ? *customBriefing : deployment->getBriefingData();
		setStandardPalette("PAL_GEOSCAPE", data.palette);
		_window->setBackground(_game->getMod()->getSurface(data.background));
		_txtCraft->setY(56 + data.textOffset);
		_txtBriefing->setY(72 + data.textOffset);
		_txtTarget->setVisible(data.showTarget);
		_txtCraft->setVisible(data.showCraft);
		_cutsceneId = data.cutscene;
		_musicId = data.music;
		if (!data.title.empty())
		{
			title = data.title;
		}
		if (!data.desc.empty())
		{
			desc = data.desc;
		}
	}
	_disableCutsceneAndMusic = _infoOnly && !customBriefing;

	add(_window, "window", "briefing");
	add(_btnOk, "button", "briefing");
	add(_txtTitle, "text", "briefing");
	add(_txtTarget, "text", "briefing");
	add(_txtCraft, "text", "briefing");
	add(_txtBriefing, "text", "briefing");

	centerAllSurfaces();

	// Set up objects
	_btnOk->setText(tr("STR_OK"));
	_btnOk->onMouseClick((ActionHandler)&BriefingState::btnOkClick);
	_btnOk->onKeyboardPress((ActionHandler)&BriefingState::btnOkClick, Options::keyOk);
	_btnOk->onKeyboardPress((ActionHandler)&BriefingState::btnOkClick, Options::keyCancel);

	_txtTitle->setBig();
	_txtTarget->setBig();
	_txtCraft->setBig();

	std::string s;
	if (craft)
	{
		if (craft->getDestination())
		{
			s = craft->getDestination()->getName(_game->getLanguage());
			battleSave->setMissionTarget(s);
		}

		s = tr("STR_CRAFT_").arg(craft->getName(_game->getLanguage()));
		battleSave->setMissionCraftOrBase(s);
	}
	else if (base)
	{
		s = tr("STR_BASE_UC_").arg(base->getName());
		battleSave->setMissionCraftOrBase(s);
	}

	// random operation names
	if (craft || base)
	{
		if (!_game->getMod()->getOperationNamesFirst().empty())
		{
			std::ostringstream ss;
			int pickFirst = RNG::seedless(0, _game->getMod()->getOperationNamesFirst().size() - 1);
			ss << _game->getMod()->getOperationNamesFirst().at(pickFirst);
			if (!_game->getMod()->getOperationNamesLast().empty())
			{
				int pickLast = RNG::seedless(0, _game->getMod()->getOperationNamesLast().size() - 1);
				ss << " " << _game->getMod()->getOperationNamesLast().at(pickLast);
			}
			s = ss.str();
			battleSave->setMissionTarget(s);
		}
	}

	if (!_game->getMod()->getOperationNamesFirst().empty())
		_txtTarget->setText(tr("STR_OPERATION_UC").arg(battleSave->getMissionTarget()));
	else
		_txtTarget->setText(battleSave->getMissionTarget());

	_txtCraft->setText(battleSave->getMissionCraftOrBase());

	_txtTitle->setText(tr(title));

	bool isPreview = battleSave->isPreview();
	if (isPreview)
	{
		if (battleSave->getCraftForPreview())
		{
			if (battleSave->getCraftForPreview()->getId() == RuleCraft::DUMMY_CRAFT_ID)
			{
				// we're using the same alienDeployment for the real craft preview and for the dummy craft preview,
				// but we want to have different briefing texts
				desc = desc + "_DUMMY";
			}
		}
		else
		{
			// base preview
			desc = desc + "_PREVIEW";
		}
	}
	_txtBriefing->setWordWrap(true);
	_txtBriefing->setText(tr(desc));

	if (_infoOnly) return;

	if (!isPreview && base && mission == "STR_BASE_DEFENSE")
	{
		auto* am = base->getRetaliationMission();

		// And make sure the base is unmarked (but only for vanilla retaliations, not for instant retaliations)
		if (am)
		{
			base->setRetaliationTarget(false);
		}

		if (am && am->getRules().isMultiUfoRetaliation())
		{
			// Remember that more UFOs may be coming
			am->setMultiUfoRetaliationInProgress(true);
		}
	}

}

/**
 *
 */
BriefingState::~BriefingState()
{

}

void BriefingState::init()
{
	State::init();
	if (_disableCutsceneAndMusic) return;

	if (!_cutsceneId.empty())
	{
		_game->pushState(new CutsceneState(_cutsceneId));

		// don't play the cutscene again when we return to this state
		_cutsceneId = "";
	}
	else
	{
		_game->getMod()->playMusic(_musicId);
	}
}

void BriefingState::loadCoop()
{

	
	// coop client inventoy fix
	if (_game->getCoopMod()->getCoopStatic() == true)
	{

		BattleUnit* selected_unit = 0;

		for (auto* unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
		{

			// fix
			if (unit->getTile())
			{

				if (!unit->getTile()->getInventory()->empty() && unit->getFaction() == FACTION_PLAYER)
				{

					selected_unit = unit;

					break;
				}

			}

	
		}

		if (!selected_unit)
		{

			for (auto* unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
			{

				if (unit->getFaction() == FACTION_PLAYER)
				{

					selected_unit = unit;

					break;
				}
			}

			if (selected_unit)
			{

				Base *selected_base = _game->getSavedGame()->getSelectedBase();

				if (!selected_base)
				{
					selected_base = _game->getSavedGame()->getBases()->front();
				}

				Craft* temp_craft = selected_base->getCrafts()->at(0);

				if (!temp_craft)
				{
					temp_craft = _game->getCoopMod()->getSelectedCraft();
				}

				auto contents = temp_craft->getExtraItems()->getContents();
				if (contents->empty())
					contents = temp_craft->getItems()->getContents();

				if (!contents->empty())
				{

					for (auto& pair : *contents)
					{
						const OpenXcom::RuleItem* ruleItem = pair.first;

						for (auto* item : *_game->getSavedGame()->getSavedBattle()->getItems())
						{
							if (item->getRules() == ruleItem && item->getSlot() && item->getSlot()->getType() == INV_GROUND)
							{

								// Check if the item already exists in the tile (to avoid duplicates)
								bool itemExists = false;
								for (auto* existingItem : *selected_unit->getTile()->getInventory())
								{
									if (existingItem->getRules() == item->getRules() && existingItem->getId() == item->getId())
									{
										itemExists = true;
										break;
									}
								}

								// Add the item only if it doesn't already exist in the tile
								if (!itemExists)
								{

									selected_unit->getTile()->addItem(item, item->getSlot());
								}
							}
						}
					}
				}
			}
		}

		if (selected_unit)
		{

			for (auto* unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
			{

				// Check if the unit has a tile before adding the item
				if (unit->getTile())
				{

					if (unit->getFaction() == FACTION_PLAYER && unit != selected_unit && unit->getTile()->getInventory()->empty())
					{

						unit->setInventoryTile(selected_unit->getTile());
					}
				}
			}
		}
	}

}

void BriefingState::setupCoop()
{

	// COOP
	if (_game->getSavedGame()->getCoop() != 0 && _game->getCoopMod()->getHost() == true && _game->getSavedGame()->getSavedBattle()->isPreview() == false)
	{

		// check if campaign mission
		if (!_game->getSavedGame()->getCountries()->empty())
		{

			_game->getCoopMod()->setCoopCampaign(true);
		}
		else
		{

			_game->getCoopMod()->setCoopCampaign(false);
		}

		// if not coop campaign
		if (_game->getCoopMod()->getCoopCampaign() == false)
		{

			// if pve2 gamemode
			if (_game->getSavedGame()->getCoop()->getGameMode() == 4)
			{

				_game->getSavedGame()->getSelectedBase()->getSoldiers()->clear();

				Waypoint* wp = new Waypoint();
				wp->setLongitude(0);
				wp->setLatitude(0);
				wp->setId(0);

				_game->getSavedGame()->getSavedBattle()->setMissionCraftOrBase("BASE> ");

				for (auto& ufo : *_game->getSavedGame()->getUfos())
				{

					ufo->setDestination(wp);
				}

				// swapper
				for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					if (unit->getFaction() == FACTION_HOSTILE)
					{

						unit->convertToFaction(FACTION_PLAYER);
						unit->setOriginalFaction(FACTION_PLAYER);
						_game->getSavedGame()->getSavedBattle()->setSelectedUnit(unit);
						unit->setAIModule(0);
					}
					else if (unit->getFaction() == FACTION_PLAYER)
					{

						unit->convertToFaction(FACTION_HOSTILE);
						unit->setOriginalFaction(FACTION_HOSTILE);
					}
				}

				// Split the soldiers in half
				if (_game->getSavedGame()->getSavedBattle())
				{

					int soldier_total_count = 0;

					// check soldiers count
					for (auto& entity : *_game->getSavedGame()->getSavedBattle()->getUnits())
					{

						if (entity->getFaction() == FACTION_PLAYER)
						{
							soldier_total_count++;
						}
					}

					int soldier_used = (soldier_total_count / 2);

					// make coop soldiers
					for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
					{

						if (unit->getFaction() == FACTION_PLAYER)
						{

							unit->setCoop(1);

							if (soldier_used <= 0)
							{
								break;
							}

							soldier_used--;
						}
					}
				}
			}
			// if pvp gamemode
			else if (_game->getSavedGame()->getCoop()->getGameMode() == 2)
			{

				for (auto* unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					if (unit->getFaction() == FACTION_HOSTILE)
					{

						unit->setCoop(1);
						unit->convertToFaction(FACTION_PLAYER);
						unit->setOriginalFaction(FACTION_PLAYER);
					}
				}
			}
			// pvp2
			else if (_game->getSavedGame()->getCoop()->getGameMode() == 3)
			{

				for (auto* unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					if (unit->getFaction() == FACTION_HOSTILE)
					{

						unit->setCoop(0);
						unit->convertToFaction(FACTION_PLAYER);
						unit->setOriginalFaction(FACTION_PLAYER);
					}
					else if (unit->getFaction() == FACTION_PLAYER)
					{

						unit->setCoop(1);
					}
				}
			}
		}

		OutputDebugStringA("BriefingState");

		_game->getCoopMod()->sendMissionFile();
	}
}

/**
 * Closes the window.
 * @param action Pointer to an action.
 */
void BriefingState::btnOkClick(Action *)
{
	_game->popState();
	Options::baseXResolution = Options::baseXBattlescape;
	Options::baseYResolution = Options::baseYBattlescape;
	_game->getScreen()->resetDisplay(false);
	if (_infoOnly) return;

	BattlescapeState *bs = new BattlescapeState;
	bs->getBattleGame()->spawnFromPrimedItems();
	BattlescapeTally tally = bs->getBattleGame()->tallyUnits();
	bool isPreview = _game->getSavedGame()->getSavedBattle()->isPreview();
	// coop
	if ((tally.liveAliens > 0 || isPreview) || connectionTCP::_coopGamemode == 2 || connectionTCP::_coopGamemode == 3 || connectionTCP::_coopGamemode == 4)
	{
		_game->pushState(bs);
		_game->getSavedGame()->getSavedBattle()->setBattleState(bs);
		_game->pushState(new NextTurnState(_game->getSavedGame()->getSavedBattle(), bs));
		if (isPreview)
		{
			// skip InventoryState
			_game->getSavedGame()->getSavedBattle()->startFirstTurn();
			return;
		}
		_game->pushState(new InventoryState(false, bs, 0));
	}
	else
	{
		Options::baseXResolution = Options::baseXGeoscape;
		Options::baseYResolution = Options::baseYGeoscape;
		_game->getScreen()->resetDisplay(false);
		delete bs;
		_game->pushState(new AliensCrashState);
	}
}

}
