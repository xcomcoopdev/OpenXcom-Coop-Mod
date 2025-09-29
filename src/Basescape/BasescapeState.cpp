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
#include "BasescapeState.h"
#include "../Engine/Game.h"
#include "../Mod/Mod.h"
#include "../Engine/LocalizedText.h"
#include "../Engine/Options.h"
#include "../Engine/Unicode.h"
#include "../Interface/TextButton.h"
#include "../Interface/Text.h"
#include "../Interface/TextEdit.h"
#include "BaseView.h"
#include "MiniBaseView.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/Base.h"
#include "../Savegame/BaseFacility.h"
#include "../Mod/RuleBaseFacility.h"
#include "../Savegame/Region.h"
#include "../Mod/RuleRegion.h"
#include "../Menu/ErrorMessageState.h"
#include "DismantleFacilityState.h"
#include "../Geoscape/BuildNewBaseState.h"
#include "../Engine/Action.h"
#include "BaseInfoState.h"
#include "SoldiersState.h"
#include "CraftsState.h"
#include "BuildFacilitiesState.h"
#include "ResearchState.h"
#include "ManageAlienContainmentState.h"
#include "ManufactureState.h"
#include "PurchaseState.h"
#include "SellState.h"
#include "TransferBaseState.h"
#include "CraftInfoState.h"
#include "../Geoscape/AllocatePsiTrainingState.h"
#include "../Geoscape/AllocateTrainingState.h"
#include "../Mod/RuleInterface.h"
#include "PlaceFacilityState.h"
#include "../Ufopaedia/Ufopaedia.h"
#include "../Battlescape/BattlescapeGenerator.h"
#include "../Battlescape/BriefingState.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Geoscape/Globe.h"
#include "../Mod/RuleGlobe.h"

#include "../Menu/LoadGameState.h"
#include "CraftSoldiersState.h"
#include "../Savegame/Vehicle.h"

namespace OpenXcom
{

bool _coop_base_init = false;

/**
 * Initializes all the elements in the Basescape screen.
 * @param game Pointer to the core game.
 * @param base Pointer to the base to get info from.
 * @param globe Pointer to the Geoscape globe.
 */
BasescapeState::BasescapeState(Base *base, Globe *globe) : _base(base), _globe(globe)
{

	// coop
	if (_game->getCoopMod()->getCoopStatic() == true && _base->_coopBase == false && _game->getCoopMod()->getCoopCampaign() == true)
	{

		// coop
		std::vector<Base*> filteredBases;

		base->old_bases = *_game->getSavedGame()->getBases();

		for (auto* base : *_game->getSavedGame()->getBases())
		{
			if (base->_coopIcon == false)
			{
				filteredBases.push_back(base);
			}
		}

		*_game->getSavedGame()->getBases() = filteredBases;

	}
	
	// Create objects
	_txtFacility = new Text(192, 9, 0, 0);
	_view = new BaseView(192, 192, 0, 8);
	_mini = new MiniBaseView(128, 16, 192, 41);
	_edtBase = new TextEdit(this, 127, 17, 193, 0);
	_txtLocation = new Text(126, 9, 194, 16);
	_txtFunds = new Text(126, 9, 194, 24);
	_btnNewBase = new TextButton(128, 12, 192, 58);
	_btnBaseInfo = new TextButton(128, 12, 192, 71);
	_btnSoldiers = new TextButton(128, 12, 192, 84);
	_btnCrafts = new TextButton(128, 12, 192, 97);
	_btnFacilities = new TextButton(128, 12, 192, 110);
	_btnResearch = new TextButton(128, 12, 192, 123);
	_btnManufacture = new TextButton(128, 12, 192, 136);
	_btnTransfer = new TextButton(128, 12, 192, 149);
	_btnPurchase = new TextButton(128, 12, 192, 162);
	_btnSell = new TextButton(128, 12, 192, 175);
	_btnGeoscape = new TextButton(128, 12, 192, 188);

	// Set palette
	setInterface("basescape");

	add(_view, "baseView", "basescape");
	add(_mini, "miniBase", "basescape");
	add(_txtFacility, "textTooltip", "basescape");
	add(_edtBase, "text1", "basescape");
	add(_txtLocation, "text2", "basescape");
	add(_txtFunds, "text3", "basescape");
	add(_btnNewBase, "button", "basescape");
	add(_btnBaseInfo, "button", "basescape");
	add(_btnSoldiers, "button", "basescape");
	add(_btnCrafts, "button", "basescape");
	add(_btnFacilities, "button", "basescape");
	add(_btnResearch, "button", "basescape");
	add(_btnManufacture, "button", "basescape");
	add(_btnTransfer, "button", "basescape");
	add(_btnPurchase, "button", "basescape");
	add(_btnSell, "button", "basescape");
	add(_btnGeoscape, "button", "basescape");

	centerAllSurfaces();

	// Set up objects
	auto* itf = _game->getMod()->getInterface("basescape")->getElement("trafficLights");
	if (itf)
	{
		_view->setOtherColors(itf->color, itf->color2, itf->border, !itf->TFTDMode);
	}
	_view->setTexture(_game->getMod()->getSurfaceSet("BASEBITS.PCK"));
	_view->onMouseClick((ActionHandler)&BasescapeState::viewLeftClick, SDL_BUTTON_LEFT);
	_view->onMouseClick((ActionHandler)&BasescapeState::viewRightClick, SDL_BUTTON_RIGHT);
	_view->onMouseClick((ActionHandler)&BasescapeState::viewMiddleClick, SDL_BUTTON_MIDDLE);
	_view->onMouseOver((ActionHandler)&BasescapeState::viewMouseOver);
	_view->onMouseOut((ActionHandler)&BasescapeState::viewMouseOut);

	_mini->setTexture(_game->getMod()->getSurfaceSet("BASEBITS.PCK"));
	_mini->setBases(_game->getSavedGame()->getBases());

	_mini->onMouseClick((ActionHandler)&BasescapeState::miniLeftClick, SDL_BUTTON_LEFT);
	_mini->onMouseClick((ActionHandler)&BasescapeState::miniRightClick, SDL_BUTTON_RIGHT);
	_mini->onKeyboardPress((ActionHandler)&BasescapeState::handleKeyPress);

	_edtBase->setBig();
	_edtBase->onChange((ActionHandler)&BasescapeState::edtBaseChange);

	_btnNewBase->setText(tr("STR_BUILD_NEW_BASE_UC"));
	_btnNewBase->onMouseClick((ActionHandler)&BasescapeState::btnNewBaseClick);
	_btnNewBase->onKeyboardPress((ActionHandler)&BasescapeState::btnNewBaseClick, Options::keyBasescapeBuildNewBase);

	_btnBaseInfo->setText(tr("STR_BASE_INFORMATION"));
	_btnBaseInfo->onMouseClick((ActionHandler)&BasescapeState::btnBaseInfoClick);
	_btnBaseInfo->onKeyboardPress((ActionHandler)&BasescapeState::btnBaseInfoClick, Options::keyBasescapeBaseInfo);

	_btnSoldiers->setText(tr("STR_SOLDIERS_UC"));
	_btnSoldiers->onMouseClick((ActionHandler)&BasescapeState::btnSoldiersClick);
	_btnSoldiers->onKeyboardPress((ActionHandler)&BasescapeState::btnSoldiersClick, Options::keyBasescapeSoldiers);

	_btnCrafts->setText(tr("STR_EQUIP_CRAFT"));
	_btnCrafts->onMouseClick((ActionHandler)&BasescapeState::btnCraftsClick);
	_btnCrafts->onKeyboardPress((ActionHandler)&BasescapeState::btnCraftsClick, Options::keyBasescapeCrafts);

	_btnFacilities->setText(tr("STR_BUILD_FACILITIES"));
	_btnFacilities->onMouseClick((ActionHandler)&BasescapeState::btnFacilitiesClick);
	_btnFacilities->onKeyboardPress((ActionHandler)&BasescapeState::btnFacilitiesClick, Options::keyBasescapeFacilities);

	_btnResearch->setText(tr("STR_RESEARCH"));
	_btnResearch->onMouseClick((ActionHandler)&BasescapeState::btnResearchClick);
	_btnResearch->onKeyboardPress((ActionHandler)&BasescapeState::btnResearchClick, Options::keyBasescapeResearch);

	_btnManufacture->setText(tr("STR_MANUFACTURE"));
	_btnManufacture->onMouseClick((ActionHandler)&BasescapeState::btnManufactureClick);
	_btnManufacture->onKeyboardPress((ActionHandler)&BasescapeState::btnManufactureClick, Options::keyBasescapeManufacture);

	_btnTransfer->setText(tr("STR_TRANSFER_UC"));
	_btnTransfer->onMouseClick((ActionHandler)&BasescapeState::btnTransferClick);
	_btnTransfer->onKeyboardPress((ActionHandler)&BasescapeState::btnTransferClick, Options::keyBasescapeTransfer);

	_btnPurchase->setText(tr("STR_PURCHASE_RECRUIT"));
	_btnPurchase->onMouseClick((ActionHandler)&BasescapeState::btnPurchaseClick);
	_btnPurchase->onKeyboardPress((ActionHandler)&BasescapeState::btnPurchaseClick, Options::keyBasescapePurchase);

	_btnSell->setText(tr("STR_SELL_SACK_UC"));
	_btnSell->onMouseClick((ActionHandler)&BasescapeState::btnSellClick);
	_btnSell->onKeyboardPress((ActionHandler)&BasescapeState::btnSellClick, Options::keyBasescapeSell);

	_btnGeoscape->setText(tr("STR_GEOSCAPE_UC"));
	_btnGeoscape->onMouseClick((ActionHandler)&BasescapeState::btnGeoscapeClick);
	_btnGeoscape->onKeyboardPress((ActionHandler)&BasescapeState::btnGeoscapeClick, Options::keyCancel);


	// COOP
	if (_base->_coopBase == true)
	{
		//_edtBase->setVisible(false);
		_btnNewBase->setVisible(false);
		//_btnBaseInfo->setVisible(false);
		//_btnSoldiers->setVisible(false);

		//_btnCrafts->setVisible(false);
		_btnFacilities->setVisible(false);
		_btnResearch->setVisible(false);
		_btnManufacture->setVisible(false);
		_btnTransfer->setVisible(false);
		_btnPurchase->setVisible(true);
		_btnSell->setVisible(false);
	}

}

/**
 *
 */
BasescapeState::~BasescapeState()
{
	// Clean up any temporary bases
	bool exists = false;
	for (const auto* xbase : *_game->getSavedGame()->getBases())
	{
		if (xbase == _base)
		{
			exists = true;
			break;
		}
	}
	if (!exists)
	{
		delete _base;
	}
}

/**
 * The player can change the selected base
 * or change info on other screens.
 */
void BasescapeState::init()
{
	State::init();

	// coop fix
	if (_game->getCoopMod()->getCoopStatic() == true && _game->getCoopMod()->getCoopCampaign() == true && _coop_base_init == false)
	{

		_coop_base_init = true;

		for (auto* soldier : *_base->getSoldiers())
		{

			if (soldier->getCoopBase() != -1 && _base->_coopBase == false)
			{

				soldier->setCraft(nullptr);
			}
			else if (_base->_coopBase == true)
			{

				// Try to assign the correct craft based on CoopCraft and CoopCraftType
				for (auto* craft : *_base->getCrafts())
				{
					if (soldier->getCoopCraft() == craft->getId() &&
						soldier->getCoopCraftType() == craft->getRules()->getType() &&
						soldier->getCoopBase() == _base->_coop_base_id)
					{
						soldier->setCraftAndMoveEquipment(craft, _base, _game->getSavedGame()->getMonthsPassed() == -1);
						break;
					}
				}
			}
		}
	}



	setBase(_base);
	_view->setBase(_base);
	_mini->draw();
	_edtBase->setText(_base->getName());

	// Get area
	for (const auto* region : *_game->getSavedGame()->getRegions())
	{
		if (region->getRules()->insideRegion(_base->getLongitude(), _base->getLatitude()))
		{
			_txtLocation->setText(tr(region->getRules()->getType()));
			break;
		}
	}

	_txtFunds->setText(tr("STR_FUNDS").arg(Unicode::formatFunding(_game->getSavedGame()->getFunds())));

	_btnNewBase->setVisible(_game->getSavedGame()->getBases()->size() < MiniBaseView::MAX_BASES);

	if (!_game->getMod()->getNewBaseUnlockResearch().empty())
	{
		bool newBasesUnlocked = _game->getSavedGame()->isResearched(_game->getMod()->getNewBaseUnlockResearch(), true);
		if (!newBasesUnlocked)
		{
			_btnNewBase->setVisible(false);
		}
	}



	// if own coop base
	if (_game->getCoopMod()->getCoopStatic() == true && _base->_coopBase == true)
	{

		_game->getCoopMod()->playerInsideCoopBase = true;

	}




}

/**
 * Changes the base currently displayed on screen.
 * @param base Pointer to new base to display.
 */
void BasescapeState::setBase(Base *base)
{
	if (!_game->getSavedGame()->getBases()->empty())
	{
		// Check if base still exists
		bool exists = false;
		for (size_t i = 0; i < _game->getSavedGame()->getBases()->size(); ++i)
		{
			if (_game->getSavedGame()->getBases()->at(i) == base && base->_coopIcon == false) // && base->_coopIcon == false
			{
				_base = base;
				_mini->setSelectedBase(i);
				_game->getSavedGame()->setSelectedBase(i);
				exists = true;
				break;
			}
		}
		// If base was removed, select first one
		if (!exists)
		{
			_base = _game->getSavedGame()->getBases()->front();
			_mini->setSelectedBase(0);
			_game->getSavedGame()->setSelectedBase(0);
		}
	}
	else
	{
		// Use a blank base for special case when player has no bases
		_base = new Base(_game->getMod());
		_mini->setSelectedBase(0);
		_game->getSavedGame()->setSelectedBase(0);
	}
}

/**
 * Goes to the Build New Base screen.
 * @param action Pointer to an action.
 */
void BasescapeState::btnNewBaseClick(Action *)
{

	// coop
	if (_base->_coopBase == true)
	{
		return;
	}

	Base *base = new Base(_game->getMod());
	_game->popState();
	_game->pushState(new BuildNewBaseState(base, _globe, false));
}

/**
 * Goes to the Base Info screen.
 * @param action Pointer to an action.
 */
void BasescapeState::btnBaseInfoClick(Action *)
{
	_game->pushState(new BaseInfoState(_base, this));
}

/**
 * Goes to the Soldiers screen.
 * @param action Pointer to an action.
 */
void BasescapeState::btnSoldiersClick(Action *)
{
	_game->pushState(new SoldiersState(_base));
}

/**
 * Goes to the Crafts screen.
 * @param action Pointer to an action.
 */
void BasescapeState::btnCraftsClick(Action *)
{
	_game->pushState(new CraftsState(_base));
}

/**
 * Opens the Build Facilities window.
 * @param action Pointer to an action.
 */
void BasescapeState::btnFacilitiesClick(Action *)
{
	_game->pushState(new BuildFacilitiesState(_base, this));
}

/**
 * Goes to the Research screen.
 * @param action Pointer to an action.
 */
void BasescapeState::btnResearchClick(Action *)
{
	_game->pushState(new ResearchState(_base));
}

/**
 * Goes to the Manufacture screen.
 * @param action Pointer to an action.
 */
void BasescapeState::btnManufactureClick(Action *)
{
	_game->pushState(new ManufactureState(_base));
}

/**
 * Goes to the Purchase screen.
 * @param action Pointer to an action.
 */
void BasescapeState::btnPurchaseClick(Action *)
{
	_game->pushState(new PurchaseState(_base));
}

/**
 * Goes to the Sell screen.
 * @param action Pointer to an action.
 */
void BasescapeState::btnSellClick(Action *)
{
	_game->pushState(new SellState(_base, 0));
}

/**
 * Goes to the Select Destination Base window.
 * @param action Pointer to an action.
 */
void BasescapeState::btnTransferClick(Action *)
{
	_game->pushState(new TransferBaseState(_base, nullptr));
}

/**
 * Returns to the previous screen.
 * @param action Pointer to an action.
 */
void BasescapeState::btnGeoscapeClick(Action *)
{

	// coop
	_coop_base_init = false;
	_game->getCoopMod()->playerInsideCoopBase = false;

	// coop
	if (_game->getCoopMod()->getCoopStatic() == true && _base->_coopBase == false)
	{
		// coop
		*_game->getSavedGame()->getBases() = _base->old_bases;

		_base->old_bases.clear();
	}

	if (_base->_coopBase == true)
	{
		_game->popState();

		if (_game->getCoopMod()->getServerOwner() == true)
		{
			_game->pushState(new LoadGameState(OPT_GEOSCAPE, "host/basehost.data", _palette));
		}
		else
		{

			_game->pushState(new LoadGameState(OPT_GEOSCAPE, "client/basehost.data", _palette));
		}
	}
	else
	{
		_game->popState();
	}

}

/**
 * Processes clicking on facilities.
 * @param action Pointer to an action.
 */
void BasescapeState::viewLeftClick(Action *)
{
	BaseFacility *fac = _view->getSelectedFacility();
	if (fac != 0)
	{
		if (_game->isCtrlPressed() && Options::isPasswordCorrect())
		{

			// coop
			if (_base->_coopBase == true)
			{
				return;
			}

			// Ctrl + left click on a base facility allows moving it
			_game->pushState(new PlaceFacilityState(_base, fac->getRules(), fac));
		}
		else
		{
			if (fac->getRules()->isLift() && _base->getFacilities()->size() > 1)
			{
				// Note: vehicles will not be deployed in the base preview
				if (_base->getAvailableSoldiers(true, true) > 0/* || !_base->getVehicles()->empty()*/)
				{
					int texture, shade;
					_globe->getPolygonTextureAndShade(_base->getLongitude(), _base->getLatitude(), &texture, &shade);
					auto* globeTexture = _game->getMod()->getGlobe()->getTexture(texture);

					SavedBattleGame* bgame = new SavedBattleGame(_game->getMod(), _game->getLanguage(), true);
					_game->getSavedGame()->setBattleGame(bgame);
					BattlescapeGenerator bgen = BattlescapeGenerator(_game);
					bgame->setMissionType("STR_BASE_DEFENSE");
					bgen.setBase(_base);
					bgen.setWorldTexture(globeTexture, globeTexture);
					bgen.run();

					_game->pushState(new BriefingState(0, _base));
				}
				return;
			}
			int errorColor1 = _game->getMod()->getInterface("basescape")->getElement("errorMessage")->color;
			int errorColor2 = _game->getMod()->getInterface("basescape")->getElement("errorPalette")->color;
			// Is facility in use?
			if (BasePlacementErrors placementErrorCode = fac->inUse())
			{
				switch (placementErrorCode)
				{
				case BPE_Used_Stores:
					_game->pushState(new ErrorMessageState(tr("STR_FACILITY_IN_USE_STORAGE"), _palette, errorColor1, "BACK13.SCR", errorColor2));
					break;
				case BPE_Used_Quarters:
					_game->pushState(new ErrorMessageState(tr("STR_FACILITY_IN_USE_QUARTERS"), _palette, errorColor1, "BACK13.SCR", errorColor2));
					break;
				case BPE_Used_Laboratories:
					_game->pushState(new ErrorMessageState(tr("STR_FACILITY_IN_USE_LABORATORIES"), _palette, errorColor1, "BACK13.SCR", errorColor2));
					break;
				case BPE_Used_Workshops:
					_game->pushState(new ErrorMessageState(tr("STR_FACILITY_IN_USE_WORKSHOPS"), _palette, errorColor1, "BACK13.SCR", errorColor2));
					break;
				case BPE_Used_Hangars:
					_game->pushState(new ErrorMessageState(tr("STR_FACILITY_IN_USE_HANGARS"), _palette, errorColor1, "BACK13.SCR", errorColor2));
					break;
				case BPE_Used_PsiLabs:
					_game->pushState(new ErrorMessageState(tr("STR_FACILITY_IN_USE_PSI_LABS"), _palette, errorColor1, "BACK13.SCR", errorColor2));
					break;
				case BPE_Used_Gyms:
					_game->pushState(new ErrorMessageState(tr("STR_FACILITY_IN_USE_GYMS"), _palette, errorColor1, "BACK13.SCR", errorColor2));
					break;
				case BPE_Used_AlienContainment:
					_game->pushState(new ErrorMessageState(tr("STR_FACILITY_IN_USE_PRISONS"), _palette, errorColor1, "BACK13.SCR", errorColor2));
					break;
				default:
					_game->pushState(new ErrorMessageState(tr("STR_FACILITY_IN_USE"), _palette, errorColor1, "BACK13.SCR", errorColor2));
				}
			}
			// Would base become disconnected?
			else if (!_base->getDisconnectedFacilities(fac).empty() && fac->getRules()->getLeavesBehindOnSell().size() == 0)
			{
				_game->pushState(new ErrorMessageState(tr("STR_CANNOT_DISMANTLE_FACILITY"), _palette, errorColor1, "BACK13.SCR", errorColor2));
			}
			// Is this facility being built from a dismantled one or building over a previous building?
			else if (fac->getBuildTime() > 0 && fac->getIfHadPreviousFacility())
			{
				_game->pushState(new ErrorMessageState(tr("STR_CANNOT_DISMANTLE_FACILITY_UPGRADING"), _palette, errorColor1, "BACK13.SCR", errorColor2));
			}
			else
			{

				// coop
				if (_base->_coopBase == true)
				{
					return;
				}

				_game->pushState(new DismantleFacilityState(_base, _view, fac));
			}
		}
	}
}

/**
 * Processes right clicking on facilities.
 * @param action Pointer to an action.
 */
void BasescapeState::viewRightClick(Action *)
{

	// coop
	if (_base->_coopBase == true)
	{
		return;
	}

	BaseFacility *f = _view->getSelectedFacility();
	if (f == 0)
	{
		_game->pushState(new BaseInfoState(_base, this));
	}
	else if (f->getRules()->getRightClickActionType() != 0)
	{
		switch (f->getRules()->getRightClickActionType())
		{
			case 1: _game->pushState(new ManageAlienContainmentState(_base, f->getRules()->getPrisonType(), OPT_GEOSCAPE)); break;
			case 2: _game->pushState(new ManufactureState(_base)); break;
			case 3: _game->pushState(new ResearchState(_base)); break;
			case 4: _game->pushState(new AllocateTrainingState(_base)); break;
			case 5: if (Options::anytimePsiTraining) _game->pushState(new AllocatePsiTrainingState(_base)); break;
			case 6: _game->pushState(new SoldiersState(_base)); break;
			case 7: _game->pushState(new SellState(_base, 0)); break;
			default: _game->popState(); break;
		}
	}
	else if (f->getRules()->isMindShield())
	{
		if (f->getBuildTime() == 0)
		{
			f->setDisabled(!f->getDisabled());
			_view->draw();
			_mini->draw();
		}
	}
	else if (f->getRules()->getCrafts() > 0)
	{
		if (f->getCraftForDrawing() == 0)
		{
			_game->pushState(new CraftsState(_base));
		}
		else
			for (size_t craft = 0; craft < _base->getCrafts()->size(); ++craft)
			{
				if (f->getCraftForDrawing() == _base->getCrafts()->at(craft))
				{
					_game->pushState(new CraftInfoState(_base, craft));
					break;
				}
			}
	}
	else if (f->getRules()->getStorage() > 0)
	{
		_game->pushState(new SellState(_base, 0));
	}
	else if (f->getRules()->getPersonnel() > 0)
	{
		_game->pushState(new SoldiersState(_base));
	}
	else if (f->getRules()->getPsiLaboratories() > 0 && Options::anytimePsiTraining && _base->getAvailablePsiLabs() > 0)
	{
		_game->pushState(new AllocatePsiTrainingState(_base));
	}
	else if (f->getRules()->getTrainingFacilities() > 0 && _base->getAvailableTraining() > 0)
	{
		_game->pushState(new AllocateTrainingState(_base));
	}
	else if (f->getRules()->getLaboratories() > 0)
	{
		_game->pushState(new ResearchState(_base));
	}
	else if (f->getRules()->getWorkshops() > 0)
	{
		_game->pushState(new ManufactureState(_base));
	}
	else if (f->getRules()->getAliens() > 0)
	{
		_game->pushState(new ManageAlienContainmentState(_base, f->getRules()->getPrisonType(), OPT_GEOSCAPE));
	}
	else if (f->getRules()->isLift() || f->getRules()->getRadarRange() > 0)
	{
		_game->popState();
	}
}

/**
* Opens the corresponding Ufopaedia article.
* @param action Pointer to an action.
*/
void BasescapeState::viewMiddleClick(Action *)
{
	BaseFacility *f = _view->getSelectedFacility();
	if (f)
	{
		std::string articleId = f->getRules()->getType();
		Ufopaedia::openArticle(_game, articleId);
	}
}

/**
 * Displays the name of the facility the mouse is over.
 * @param action Pointer to an action.
 */
void BasescapeState::viewMouseOver(Action *)
{
	BaseFacility *f = _view->getSelectedFacility();
	std::ostringstream ss;
	if (f != 0)
	{
		if (f->getRules()->getCrafts() == 0 || f->getBuildTime() > 0)
		{
			ss << tr(f->getRules()->getType());
		}
		else
		{
			ss << tr(f->getRules()->getType());
			if (f->getCraftForDrawing() != 0)
			{
				ss << " " << tr("STR_CRAFT_").arg(f->getCraftForDrawing()->getName(_game->getLanguage()));
			}
		}
	}
	_txtFacility->setText(ss.str());
}

/**
 * Clears the facility name.
 * @param action Pointer to an action.
 */
void BasescapeState::viewMouseOut(Action *)
{
	_txtFacility->setText("");
}

/**
 * Selects a new base to display.
 * @param action Pointer to an action.
 */
void BasescapeState::miniLeftClick(Action *)
{
	size_t base = _mini->getHoveredBase();
	if (base < _game->getSavedGame()->getBases()->size())
	{
		_base = _game->getSavedGame()->getBases()->at(base);
		init();
	}
}

/**
 * Moves the current base to the left in the list of bases.
 * @param action Pointer to an action.
 */
void BasescapeState::miniRightClick(Action *)
{
	size_t baseIndex = _mini->getHoveredBase();

	if (baseIndex > 0 && baseIndex < _game->getSavedGame()->getBases()->size())
	{
		auto& bases = *_game->getSavedGame()->getBases();

		// only able to move the currently selected base
		if (bases[baseIndex] == _base)
		{
			std::swap(bases[baseIndex], bases[baseIndex - 1]);
			init();
		}
	}
}

/**
 * Selects a new base to display.
 * @param action Pointer to an action.
 */
void BasescapeState::handleKeyPress(Action *action)
{
	if (action->getDetails()->type == SDL_KEYDOWN)
	{
		SDLKey baseKeys[] = {
			Options::keyBaseSelect1,
			Options::keyBaseSelect2,
			Options::keyBaseSelect3,
			Options::keyBaseSelect4,
			Options::keyBaseSelect5,
			Options::keyBaseSelect6,
			Options::keyBaseSelect7,
			Options::keyBaseSelect8
		};
		int key = action->getDetails()->key.keysym.sym;
		for (size_t i = 0; i < _game->getSavedGame()->getBases()->size(); ++i)
		{
			if (key == baseKeys[i])
			{
				_base = _game->getSavedGame()->getBases()->at(i);
				init();
				break;
			}
		}
	}
}

/**
 * Changes the Base name.
 * @param action Pointer to an action.
 */
void BasescapeState::edtBaseChange(Action *)
{

	// coop
	if (_base->_coopBase == true)
	{
		return;
	}
	else if (_game->getCoopMod()->getCoopStatic() == true)
	{

		Json::Value root;

		root["state"] = "changeBaseName";

		root["oldName"] = _base->getName();
		root["newName"] = _edtBase->getText();

		_game->getCoopMod()->sendTCPPacketData(root.toStyledString());

	}

	_base->setName(_edtBase->getText());
}

}
