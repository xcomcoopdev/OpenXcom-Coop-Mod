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
#include "PlaceLiftState.h"
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/Sound.h"
#include "../Mod/Mod.h"
#include "../Engine/LocalizedText.h"
#include "../Interface/Text.h"
#include "../Interface/TextList.h"
#include "../Interface/Window.h"
#include "BaseView.h"
#include "../Savegame/Base.h"
#include "../Savegame/BaseFacility.h"
#include "../Mod/RuleBaseFacility.h"
#include "BasescapeState.h"
#include "SelectStartFacilityState.h"
#include "../Savegame/SavedGame.h"
#include "../Ufopaedia/Ufopaedia.h"
#include "../Mod/RuleInterface.h"
#include "../Geoscape/Globe.h"
#include "../Mod/RuleGlobe.h"
#include "../CoopMod/connectionTCP.h"
#include "../CoopMod/JointEcon.h"

namespace OpenXcom
{

/**
 * Initializes all the elements in the Place Lift screen.
 * @param game Pointer to the core game.
 * @param base Pointer to the base to get info from.
 * @param globe Pointer to the Geoscape globe.
 * @param first Is this a custom starting base?
 */
PlaceLiftState::PlaceLiftState(Base *base, Globe *globe, bool first) : _base(base), _globe(globe), _first(first)
{
	// Create objects
	_view = new BaseView(192, 192, 0, 8);
	_txtTitle = new Text(320, 9, 0, 0);
	_window = new Window(this, 128, 160, 192, 40, POPUP_NONE);
	_txtHeader = new Text(118, 17, 197, 48);
	_lstAccessLifts = new TextList(104, 104, 200, 64);

	// Set palette
	setInterface("placeFacility");

	add(_view, "baseView", "basescape");
	add(_txtTitle, "text", "placeFacility");
	add(_window, "window", "selectFacility");
	add(_txtHeader, "text", "selectFacility");
	add(_lstAccessLifts, "list", "selectFacility");

	centerAllSurfaces();

	// Set up objects
	setWindowBackground(_window, "selectFacility");

	if (_globe && _base)
	{
		int texture, shade;
		_globe->getPolygonTextureAndShade(_base->getLongitude(), _base->getLatitude(), &texture, &shade);
		auto* globeTexture = _game->getMod()->getGlobe()->getTexture(texture);
		_base->setGlobeTexture(globeTexture);
	}

	auto* itf = _game->getMod()->getInterface("basescape")->getElementOptional("trafficLights");
	if (itf)
	{
		_view->setOtherColors(itf->color, itf->color2, itf->border, !itf->TFTDMode);
	}
	_view->setTexture(_game->getMod()->getSurfaceSet("BASEBITS.PCK"));
	_view->setBase(_base);

	_lift = nullptr;
	for (auto& facilityType : _game->getMod()->getBaseFacilitiesList())
	{
		auto* facilityRule = _game->getMod()->getBaseFacility(facilityType);
		if ((facilityRule->isLift() && !facilityRule->isUpgradeOnly())
			&& facilityRule->isAllowedForBaseType(_base->isFakeUnderwater()) && _game->getSavedGame()->isResearched(facilityRule->getRequirements()))
		{
			_accessLifts.push_back(facilityRule);
		}
	}
	if (_first || _accessLifts.size() == 1)
	{
		_lift = _accessLifts.front();
	}

	_txtHeader->setBig();
	_txtHeader->setAlign(ALIGN_CENTER);
	_txtHeader->setText(tr("STR_INSTALLATION"));

	_lstAccessLifts->setColumns(1, 104);
	_lstAccessLifts->setSelectable(true);
	_lstAccessLifts->setBackground(_window);
	_lstAccessLifts->setMargin(2);
	_lstAccessLifts->setWordWrap(true);
	_lstAccessLifts->setScrolling(true, 0);
	_lstAccessLifts->onMouseClick((ActionHandler)&PlaceLiftState::lstAccessLiftsClick);
	_lstAccessLifts->onMouseClick((ActionHandler)&PlaceLiftState::lstAccessLiftsClick, SDL_BUTTON_MIDDLE);

	for (const auto* facRule : _accessLifts)
	{
		_lstAccessLifts->addRow(1, tr(facRule->getType()).c_str());
	}

	if (_lift)
	{
		_lstAccessLifts->setVisible(false);
		_txtHeader->setVisible(false);
		_window->setVisible(false);

		_view->setSelectable(_lift->getSizeX(), _lift->getSizeY());
		_view->onMouseClick((ActionHandler)&PlaceLiftState::viewClick);
	}

	_txtTitle->setText(tr("STR_SELECT_POSITION_FOR_ACCESS_LIFT"));
}

/**
 *
 */
PlaceLiftState::~PlaceLiftState()
{

}

/**
 * Processes clicking on facilities.
 * @param action Pointer to an action.
 */
void PlaceLiftState::viewClick(Action *)
{
	// PRD-J07 JOINT: a SUBSEQUENT base is created host-authoritatively. Instead of
	// mutating the floating _base + pushing markers, submit ONE base_new command
	// carrying lon/lat/name/lift so the host builds the whole base atomically
	// (same coopbaseid, name, lift position, funds debited once) and broadcasts it.
	// The initial campaign base (_first) is J02's and stays the local/streamed path.
	if (_game->getCoopMod()->isJointCampaign() && !_first)
	{
		submitJointNewBase(_view->getGridX(), _view->getGridY());
		delete _base; // floating UI scratch base, never added to getBases()
		_game->popState();
		return;
	}

	BaseFacility *fac = new BaseFacility(_lift, _base);
	fac->setX(_view->getGridX());
	fac->setY(_view->getGridY());
	_base->getFacilities()->push_back(fac);
	if (fac->getRules()->getPlaceSound() != Mod::NO_SOUND)
	{
		_game->getMod()->getSound("GEO.CAT", fac->getRules()->getPlaceSound())->play();
	}
	_game->popState();
	BasescapeState *bState = new BasescapeState(_base, _globe);
	_game->getSavedGame()->setSelectedBase(_game->getSavedGame()->getBases()->size() - 1);
	_game->pushState(bState);
	if (_first)
	{
		_game->pushState(new SelectStartFacilityState(_base, bState, _globe));
	}

	// coop (SEPARATE mirror markers only; JOINT rides the base_new joint_cmd above)
	if (_game->getCoopMod()->getCoopStatic() == true && !_game->getCoopMod()->isJointCampaign())
	{

		// BASE
		Json::Value markers;

		markers["state"] = "new_base";

		markers["markers"]["coopbaseid"] = _base->_coop_base_id;

		markers["markers"]["base"] = _base->getName().c_str();
		markers["markers"]["lon"] = _base->getLongitude();
		markers["markers"]["lan"] = _base->getLatitude();

		markers["markers"]["getAvailableEngineers"] = _base->getAvailableEngineers();
		markers["markers"]["getAvailableHangars"] = _base->getAvailableHangars();
		markers["markers"]["getAvailableLaboratories"] = _base->getAvailableLaboratories();
		markers["markers"]["getAvailableQuarters"] = _base->getAvailableQuarters();
		markers["markers"]["getAvailableScientists"] = _base->getAvailableScientists();
		markers["markers"]["getAvailableSoldiers"] = _base->getAvailableSoldiers();
		markers["markers"]["getAvailableStores"] = _base->getAvailableStores();
		markers["markers"]["getAvailableTraining"] = _base->getAvailableTraining();
		markers["markers"]["getAvailableWorkshops"] = _base->getAvailableWorkshops();

		_game->getCoopMod()->sendTCPPacketData(markers.toStyledString());

	}


}

/**
 * Selects the access lift to place.
 * @param action Pointer to an action.
 */
void PlaceLiftState::lstAccessLiftsClick(Action *action)
{
	auto index = _lstAccessLifts->getSelectedRow();
	if (index >= _accessLifts.size())
	{
		return;
	}

	if (action->getDetails()->button.button == SDL_BUTTON_MIDDLE)
	{
		Ufopaedia::openArticle(_game, _accessLifts[index]->getType());
		return;
	}

	_lift = _accessLifts[index];

	if (_lift)
	{
		_lstAccessLifts->setVisible(false);
		_txtHeader->setVisible(false);
		_window->setVisible(false);

		_view->setSelectable(_lift->getSizeX(), _lift->getSizeY());
		_view->onMouseClick((ActionHandler)&PlaceLiftState::viewClick);
	}
}

/**
 * PRD-J07 JOINT: emit the base_new joint_cmd for a subsequent base. baseId = -1
 * (no existing base); the host validates funds + region, creates the base (minting
 * a coopbaseid it serializes into the broadcast), places the access lift, debits
 * the region cost once, and appends the base at the same index on every machine.
 */
void PlaceLiftState::submitJointNewBase(int x, int y)
{
	if (!_lift)
		_lift = _accessLifts.empty() ? nullptr : _accessLifts.front();
	if (!_lift) return;
	Json::Value payload;
	payload["lon"] = _base->getLongitude();
	payload["lat"] = _base->getLatitude();
	payload["name"] = _base->getName();
	payload["fakeUnderwater"] = _base->isFakeUnderwater();
	payload["liftType"] = _lift->getType();
	payload["liftX"] = x;
	payload["liftY"] = y;
	JointEcon::submitLocalCmd(_game, "base_new", -1, payload);
}

/**
 * Test automation: pick the (front) access lift and place it at grid (x,y) via
 * the REAL viewClick path (JOINT -> base_new joint_cmd + pop this state;
 * SEPARATE/solo -> local build). Returns false if no access lift is available.
 */
bool PlaceLiftState::harnessPlaceLift(int x, int y)
{
	if (!_lift && !_accessLifts.empty()) _lift = _accessLifts.front();
	if (!_lift) return false;
	_view->setGridPosition(x, y);
	viewClick(nullptr);
	return true;
}

}
