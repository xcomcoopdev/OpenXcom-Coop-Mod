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
#include <vector>
#include "../Savegame/SavedGame.h"
#include "../Engine/Options.h"
#include "../Menu/OptionsBaseState.h"

namespace OpenXcom
{

class TextButton;
class Window;
class Text;
class TextList;
class TextEdit;
class ArrowButton;
class ToggleTextButton;

/**
 * Base class for saved game screens which
 * provides the common layout and listing.
 */
class LobbyMenu : public State
{
protected:
	TextButton *_btnDisconnect, *_btnChat, *_btnCancel;
	Window *_window;
	Text *_txtTitle, *_txtName, *_txtLatency, *_txtDetails, *_txtTeam, *_txtChangeTeam;
	TextList *_lstPlayers;
	ArrowButton *_sortName, *_sortLatency, *_sortTeam;
	OptionsOrigin _origin;
	std::vector<playerInfo> _connectedPlayers;
	unsigned int _firstValidRow = 0;
	bool _sortable;
	bool _timerStarted = false;
	bool _redirected = false; // think() left the lobby after a lost connection
	/// Playtest B7: this coop menu was opened MID-GAME (a running campaign geoscape
	/// sits on the stack underneath). The action button becomes RESUME GAME and just
	/// returns to the live game instead of leaving disconnect as the only option.
	bool _resumeToGame = false;
	/// Playtest B7: pop the coop-menu states back down to the running game geoscape.
	void returnToRunningGame();
	int _countdown = 30; // seconds
	void updateArrows();
  public:
	/// Creates the Game state.
	LobbyMenu();
	/// Cleans up the Game state.
	virtual ~LobbyMenu();
	/// Sets up the list.
	void init() override;
	/// Sorts the server list.
	void sortList(playerSort sort);
	/// Updates the server list.
	virtual void updateList();
	/// Handler for clicking the Cancel button.
	void btnCancelClick(Action *action);
	void btnDisconnectClick(Action* action);
	/// Returns to the server browser, reusing one already in the stack.
	void pushServerListUnlessPresent();
	// Pop everything stacked above the LobbyMenu (inclusive) and mark the
	// session's lobby closed. Shared by startCampaign() and resumeCampaign().
	void closeLobby();
	/// Host pressed BATTLE SETTINGS in the skirmish lobby (mode 0).
	void openBattleSettings();
	/// Is the client's local custom-battle craft ready to be opened safely?
	bool canOpenEquipCraft() const;
	/// Client pressed EQUIP CRAFT in the skirmish lobby (mode 0).
	void openEquipCraft();
	void btnChatClick(Action* action);
	/// Handler for moving the mouse over a list item.
	void lstSavesMouseOver(Action *action);
	/// Handler for moving the mouse outside the list borders.
	void lstSavesMouseOut(Action *action);
	/// Handler for clicking the list.
	virtual void lstSavesPress(Action *action);
	/// disables the sort buttons.
	void disableSort();
	/// Runs the timers and handles popups.
	void think() override;
	/// Can the host start the new campaign (>= 1 client connected)?
	bool startEligible() const;
	/// Locks players/teams, writes the initial save and begins base building
	/// on every machine (flow-redesign F2).
	void startCampaign();
	/// PRD-10: push the real START CAMPAIGN confirm dialog (harness hook so a
	/// test can exercise the true confirm->OK path, not a startCampaign bypass).
	void openStartConfirmDialog();
	/// PRD-10: if a confirm dialog is on top, invoke its OK exactly as a click
	/// would (pops it, re-checks eligibility, starts only if still eligible).
	/// Returns true if a confirm dialog was found and clicked.
	bool clickStartConfirmOk();
	/// Registered players not yet in the lobby (resume mode, F3).
	std::vector<std::string> missingPlayers() const;
	/// The "waiting" details line for the CURRENT lobby mode. Resuming a saved co-op
	/// campaign names the players still missing ("Waiting for A, B on port N") because the
	/// roster is known; starting a NEW game has no roster yet, so it is the generic
	/// "Waiting for players on port N". Used everywhere the line is (re)written, so a
	/// mouse-over/out no longer overwrites the named form with the generic one.
	std::string waitingText() const;
	/// Serves every resuming player its world and waits for acks (F3).
	void resumeCampaign();
	/// Introspection for the test harness.
	std::string actionButtonText() const;
	bool actionButtonVisible() const;
	std::string detailsText() const;
	std::vector<std::string> rosterNames() const;
	/// Test automation: roster row name by player id (1=host row, 2=client row),
	/// unsorted (the displayed roster is name-sorted).
	std::string rowNameById(int id) const;
	/// Handler for clicking the Name arrow.
	void sortNameClick(Action* action);
	void sortLatencyClick(Action* action);
	void sortTeamClick(Action* action);

};

}
