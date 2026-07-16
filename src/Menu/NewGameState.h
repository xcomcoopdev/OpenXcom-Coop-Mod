#pragma once
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
#include "../Engine/State.h"
#include "../Savegame/SavedGame.h" // PRD-J01: CoopCampaignType for the ctor

namespace OpenXcom
{

class TextButton;
class ToggleTextButton;
class Window;
class Text;
class Game;
class GeoscapeState;
class Base;

// Shared by solo new game, host lobby start, and client campaign_start:
// centers the globe on the base marker and routes into base naming/placement.
// Null-tolerant on gs (does nothing if gs is null, mirroring the lobby guard).
void beginInitialBasePlacement(Game* game, GeoscapeState* gs, Base* base);

/**
 * New Game window that displays a list
 * of possible difficulties for creating
 * a saved game.
 */
class NewGameState : public State
{
private:
	TextButton *_btnBeginner, *_btnExperienced, *_btnVeteran, *_btnGenius, *_btnSuperhuman;
	TextButton *_difficulty;
	ToggleTextButton *_btnIronman;
	TextButton *_btnOk, *_btnCancel;
	Window *_window;
	Text *_txtTitle, *_txtIronman;
	// true = creating a co-op campaign: world is created, base placement is
	// deferred, and the flow continues into the host window + lobby
	bool _coopCampaign;
	// PRD-J01: JOINT/SEPARATE economy model, applied to the new save on OK.
	CoopCampaignType _campaignType;
public:
	/// Creates the New Game state.
	NewGameState(bool coopCampaign = false, CoopCampaignType campaignType = CoopCampaignType::Separate);
	/// Cleans up the New Game state.
	~NewGameState();
	/// Handler for clicking the Ok button.
	void btnOkClick(Action *action);
	/// Handler for clicking the Cancel button.
	void btnCancelClick(Action *action);
};

}
