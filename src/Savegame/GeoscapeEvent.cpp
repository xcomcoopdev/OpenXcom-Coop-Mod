/*
 * Copyright 2010-2019 OpenXcom Developers.
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
#include "GeoscapeEvent.h"
#include <assert.h>
#include "../Mod/RuleEvent.h"

namespace OpenXcom
{

GeoscapeEvent::GeoscapeEvent(const RuleEvent &rule) : _rule(rule), _spawnCountdown(0), _over(false)
{
	// Empty by design.
}

GeoscapeEvent::~GeoscapeEvent()
{
	// Empty by design.
}

/**
 * Loads the event from YAML.
 * @param node The YAML node containing the data.
 */
void GeoscapeEvent::load(const YAML::Node &node)
{
	_spawnCountdown = node["spawnCountdown"].as<size_t>(_spawnCountdown);
	_over = node["over"].as<bool>(_over);
}

/**
 * Saves the event to YAML.
 * @return YAML node.
 */
YAML::Node GeoscapeEvent::save() const
{
	YAML::Node node;
	node["name"] = _rule.getName();
	node["spawnCountdown"] = _spawnCountdown;
	if (_over)
	{
		node["over"] = _over;
	}
	return node;
}

/**
 * The new time must be a multiple of 30 minutes, and more than 0.
 * Calling this on a finished event has no effect.
 * @param minutes The minutes until the event will pop up.
 */
void GeoscapeEvent::setSpawnCountdown(size_t minutes)
{
	assert(minutes != 0 && minutes % 30 == 0);
	if (_over)
	{
		return;
	}
	_spawnCountdown = minutes;
}

void GeoscapeEvent::think()
{
	// if finished, don't do anything
	if (_over)
	{
		return;
	}

	// are we there yet?
	if (_spawnCountdown > 30)
	{
		_spawnCountdown -= 30;
		return;
	}

	// ok, the time has come...
	_over = true;
}

}
