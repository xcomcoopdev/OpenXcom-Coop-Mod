#pragma once
/*
 * Copyright 2010-2026 OpenXcom Developers.
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
#include <string>
#include <vector>
#include "OptionsBaseState.h" // OptionsOrigin
#include "../Savegame/Upgrade/SaveUpgrade.h" // UpgradeInputs

namespace OpenXcom
{

class Game;

/// Shared orchestration for the in-game save upgrade UI (Phase B). These free
/// functions push the appropriate follow-up state; they never write to disk
/// except through UpgradeRunner::execute().
namespace SaveUpgradeUI
{

/// Runs preflight, then routes: blocking errors -> message dialog; warnings ->
/// a Continue/Cancel confirm; clean -> straight to execute.
void beginUpgrade(Game* game, OptionsOrigin origin, const std::string& filename, const SaveUpgrade::UpgradeInputs& inputs);

/// Runs execute(): pushes the summary state on success, or an error dialog on
/// failure (the original save is left untouched by a failed execute()).
void runExecute(Game* game, OptionsOrigin origin, const std::string& filename, const SaveUpgrade::UpgradeInputs& inputs);

/// Pushes a themed single-OK information/error dialog over the given origin.
void pushMessage(Game* game, OptionsOrigin origin, const std::string& header, const std::vector<std::string>& lines);

} // namespace SaveUpgradeUI

} // namespace OpenXcom
