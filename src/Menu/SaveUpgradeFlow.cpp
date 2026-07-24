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
#include "SaveUpgradeFlow.h"
#include "../Engine/Game.h"
#include "../Engine/Language.h"
#include "../Engine/Logger.h"
#include "SaveUpgradeMessageState.h"
#include "SaveUpgradeSummaryState.h"

namespace OpenXcom
{

namespace SaveUpgradeUI
{

void pushMessage(Game* game, OptionsOrigin origin, const std::string& header, const std::vector<std::string>& lines)
{
	game->pushState(new SaveUpgradeMessageState(origin, header, lines));
}

void runExecute(Game* game, OptionsOrigin origin, const std::string& filename, const SaveUpgrade::UpgradeInputs& inputs)
{
	SaveUpgrade::UpgradeRunner runner(filename);
	SaveUpgrade::ExecuteResult r = runner.execute(inputs);
	if (r.success)
	{
		game->pushState(new SaveUpgradeSummaryState(origin, filename, inputs, r));
	}
	else
	{
		Log(LOG_ERROR) << "[save-upgrade] execute failed: " << r.errorMessage;
		std::vector<std::string> lines;
		lines.push_back(r.errorMessage);
		lines.push_back(game->getLanguage()->getString("STR_SAVE_UPGRADE_ORIGINAL_UNTOUCHED"));
		pushMessage(game, origin, game->getLanguage()->getString("STR_SAVE_UPGRADE_FAILED"), lines);
	}
}

void beginUpgrade(Game* game, OptionsOrigin origin, const std::string& filename, const SaveUpgrade::UpgradeInputs& inputs)
{
	SaveUpgrade::UpgradeRunner runner(filename);
	SaveUpgrade::PreflightResult pf = runner.preflight(inputs);

	if (!pf.ok())
	{
		// Blocking errors: cannot proceed. Phase A returns English strings; show
		// them under a localized heading (see PRD 4.4).
		pushMessage(game, origin, game->getLanguage()->getString("STR_SAVE_UPGRADE_BLOCKED"), pf.errors);
		return;
	}

	if (!pf.warnings.empty())
	{
		// Non-blocking warnings: require an explicit "Continue anyway".
		game->pushState(new SaveUpgradeMessageState(origin, filename, inputs,
			game->getLanguage()->getString("STR_SAVE_UPGRADE_WARN_INTRO"), pf.warnings));
		return;
	}

	runExecute(game, origin, filename, inputs);
}

} // namespace SaveUpgradeUI

} // namespace OpenXcom
