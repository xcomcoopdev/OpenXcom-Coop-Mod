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

#include "UnitTurnBState.h"
#include "TileEngine.h"
#include "Map.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Mod/Mod.h"
#include "../Engine/Sound.h"
#include "../Engine/Options.h"

namespace OpenXcom
{

/**
 * Sets up an UnitTurnBState.
 * @param parent Pointer to the Battlescape.
 * @param action Pointer to an action.
 */
UnitTurnBState::UnitTurnBState(BattlescapeGame *parent, BattleAction action, bool chargeTUs) : BattleState(parent, action), _unit(0), _turret(false), _chargeTUs(chargeTUs)
{

}

/**
 * Deletes the UnitTurnBState.
 */
UnitTurnBState::~UnitTurnBState()
{

}

/**
 * Deinitalize the state.
 */
void UnitTurnBState::deinit()
{

	// coop
	if (_parent->isCoop() == true && _parent->getCoopMod()->_isActivePlayerSync == true)
	{
		// Sync the unit's charged stats, but only if it actually turned this
		// state. think() is the single authoritative place that charges turn
		// TUs, so reading getTimeUnits() here (after the turn finishes) gives
		// the correct, charged-once value. The client mirrors this value and
		// replays the turn without charging again (chargeTUs == false), so each
		// side spends the turn cost exactly once.
		const bool turned = (_coopStartDirection != -1) &&
							((_coopStartDirection != _unit->getDirection()) ||
							 (_coopStartTurret != _unit->getTurretDirection()));

		if (turned)
		{
			Json::Value turn;
			turn["state"] = "turnBattlescapeUnit";
			turn["id"] = _unit->getId();

			turn["coords"]["start"]["x"] = _unit->getPosition().x;
			turn["coords"]["start"]["y"] = _unit->getPosition().y;
			turn["coords"]["start"]["z"] = _unit->getPosition().z;

			turn["coords"]["end"]["x"] = _action.target.x;
			turn["coords"]["end"]["y"] = _action.target.y;
			turn["coords"]["end"]["z"] = _action.target.z;

			turn["isActionTypeNone"] = (_action.type == BA_NONE);

			turn["tu"] = _unit->getTimeUnits();
			turn["energy"] = _unit->getEnergy();
			turn["health"] = _unit->getHealth();
			turn["morale"] = _unit->getMorale();
			turn["stunlevel"] = _unit->getStunlevel();
			turn["mana"] = _unit->getMana();

			if (_parent->getCoopGamemode() != 2 && _parent->getCoopGamemode() != 3 && _parent->getCoopMod()->_isActiveAISync == false)
			{
				int j = 0;
				for (auto* bu : *_unit->getVisibleUnits())
				{
					turn["visible_units"][j]["unit_id"] = _unit->getId();
					j++;
				}
			}

			_parent->getCoopMod()->sendTCPPacketData(turn.toStyledString());
		}

		Json::Value obj;
		obj["state"] = "afterBattlescapeUnitTurn";

		obj["setDirection"] = _unit->getDirection();
		obj["setFaceDirection"] = _unit->getFaceDirection();

		obj["setTurretDirection"] = _unit->getTurretDirection();
		obj["setTurretToDirection"] = _unit->getTurretToDirection();

		obj["unit_id"] = _unit->getId();

		_parent->getCoopMod()->sendTCPPacketData(obj.toStyledString());

	}

	// coop
	_parent->setCoopTaskCompleted(true);
}

/**
 * Initializes the state.
 */
void UnitTurnBState::init()
{

	// coop
	_parent->setCoopTaskCompleted(false);

	_unit = _action.actor;
	if (_unit->isOut())
	{
		_parent->popState();
		return;
	}

	// coop: remember the facing now so deinit() can tell whether the unit
	// actually turned (and only then sync the turn / charged stats).
	_coopStartDirection = _unit->getDirection();
	_coopStartTurret = _unit->getTurretDirection();

	_action.clearTU();
	if (_unit->getFaction() == FACTION_PLAYER)
		_parent->setStateInterval(Options::battleXcomSpeed);
	else
		_parent->setStateInterval(Options::battleAlienSpeed);

	// if the unit has a turret and we are turning during targeting, then only the turret turns
	_turret = _unit->getTurretType() != -1 && (_action.targeting || _action.strafe);

	_unit->lookAt(_action.target, _turret);

	if (_chargeTUs && _unit->getStatus() != STATUS_TURNING)
	{
		if (_action.type == BA_NONE)
		{
			// try to open a door
			int door = _parent->getTileEngine()->unitOpensDoor(_unit, true);
			if (door == 0)
			{
				_parent->getMod()->getSoundByDepth(_parent->getDepth(), Mod::DOOR_OPEN)->play(-1, _parent->getMap()->getSoundAngle(_unit->getPosition())); // normal door
			}
			if (door == 1)
			{
				_parent->getMod()->getSoundByDepth(_parent->getDepth(), Mod::SLIDING_DOOR_OPEN)->play(-1, _parent->getMap()->getSoundAngle(_unit->getPosition())); // ufo door
			}
			if (door == 4)
			{
				_action.result = "STR_NOT_ENOUGH_TIME_UNITS";
			}
		}
		_parent->popState();
	}

	// coop: the turn's TU/stats are synced from deinit() once the turn has
	// completed. think() (below) is the single place that charges turn TUs;
	// charging here as well previously doubled the cost.
}

/**
 * Runs state functionality every cycle.
 */
void UnitTurnBState::think()
{
	const int tu = _chargeTUs ? (_turret ? 1 :_unit->getTurnCost()) : 0;

	if (_chargeTUs && _unit->getFaction() == _parent->getSave()->getSide() && _parent->getPanicHandled() && !_action.targeting && !_parent->checkReservedTU(_unit, tu, 0))
	{
		_unit->abortTurn();
		_parent->popState();
		return;
	}

	if (_unit->spendTimeUnits(tu))
	{
		size_t unitSpotted = _unit->getUnitsSpottedThisTurn().size();
		_unit->turn(_turret);
		_parent->getTileEngine()->calculateFOV(_unit);
		if (_chargeTUs && _unit->getFaction() == _parent->getSave()->getSide() && _parent->getPanicHandled() && _action.type == BA_NONE && _unit->getUnitsSpottedThisTurn().size() > unitSpotted)
		{
			_unit->abortTurn();
			_parent->popState();
		}
		else if (_unit->getStatus() == STATUS_STANDING)
		{
			_parent->popState();

			if (_action.kneel && !_unit->isFloating() && !_unit->isKneeled())
			{
				BattleAction kneel;
				kneel.type = BA_KNEEL;
				kneel.actor = _unit;
				kneel.Time = _unit->getKneelChangeCost();
				if (kneel.spendTU())
				{
					_unit->kneel(!_unit->isKneeled());
					// kneeling or standing up can reveal new terrain or units. I guess.
					_parent->getTileEngine()->calculateFOV(_unit->getPosition(), 1, false); //Update unit FOV for everyone through this position, skip tiles.
					_parent->getTileEngine()->checkReactionFire(_unit, kneel);
				}
			}
		}
	}
	else if (_parent->getPanicHandled())
	{
		_action.result = "STR_NOT_ENOUGH_TIME_UNITS";
		_unit->abortTurn();
		_parent->popState();
	}
}

/**
 * Unit turning cannot be cancelled.
 */
void UnitTurnBState::cancel()
{
}

}
