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
#include "../Menu/OptionsBaseState.h"
#include "../Engine/Screen.h"
#include "../Geoscape/Globe.h"

namespace OpenXcom
{

class TextButton;
class Window;
class Text;
class GeoscapeState;

/**
 * Named dialog codes for CoopState. CoopState is a multi-purpose wait/prompt
 * dialog selected by a raw int passed to its constructor. Explicit values so
 * nothing renumbers (the ints appear in logs and harness tests).
 *
 * Only the codes that participate in the campaign-wait lifecycle (and the two
 * save/load wait dialogs) are named here so far; the remaining placeholder
 * codes scattered through the coop states are still raw literals pending a
 * future documentation pass (out of scope for PRD-01).
 */
enum CoopDialogCode {
	COOP_DLG_CLIENT_LOAD_WAIT = 52, // client "loading" wait
	COOP_DLG_HOST_SAVE_WAIT   = 54, // host "saving" wait
	COOP_DLG_WAIT_BASES       = 60, // host waits for every client to place a base
	COOP_DLG_RESUME_ACK_WAIT  = 62, // host waits for resuming players to ack
	COOP_DLG_FREEZE           = 64, // mid-session freeze: a player dropped
	COOP_DLG_CLIENT_HOLD      = 65, // client placed base, holds until host resumes
	COOP_DLG_CLIENT_RESUME_HOLD = 68, // rejoined client holds until host resumes
	COOP_DLG_JOINT_FAIL       = 556, // PRD-J10: the host rejected a JOINT command
};

/**
 * Options window shown for loading/saving/quitting the game.
 * Not to be confused with the Game Options window
 * for changing game settings during runtime.
 */
class CoopState : public State
{
  private:
	OptionsOrigin _origin;
	Window *_window;
	Text *_txtTitle;
	TextButton *_btnMessage, *_btnBack, *_btnYes;
	int global_state = 0;
	int state_counter = 0;
	// PRD-11 C13: retry bookkeeping for the client load-wait dialog (52). When
	// the host replies "busy", wait ~2s (_loadWaitTicks at the 500ms gate) then
	// re-send request_load_progress, up to a bounded number of retries.
	int _loadRetries = 0;
	int _loadWaitTicks = 0;
  public:
	/// Creates the Pause state.
	CoopState(int state);
	/// Cleans up the Pause state.
	~CoopState();
	void loadCoop(Action *);
	void previous(Action *);
	void btnYesClick(Action *);
	void loadWorld();
	void setGlobe(Globe *globe);
	void setBaseName(std::string name);
	/// Which dialog this is (see the state-code blocks in the constructor).
	int getStateCode() const { return global_state; }
	/// Introspection for the test harness: the current title / back-button text
	/// and whether the back button is visible.
	std::string getTitleText() const;
	std::string getBackText() const;
	bool isBackVisible() const;
	int getWindowHeight() const;
	/// True for the campaign-wait family (60/62/64/65/67): dialogs that manage
	/// their own lifetime and must never be popped by save/load-progress handlers.
	bool isCampaignWaitDialog() const
	{
		return global_state == COOP_DLG_WAIT_BASES
			|| global_state == COOP_DLG_RESUME_ACK_WAIT
			|| global_state == COOP_DLG_FREEZE
			|| global_state == COOP_DLG_CLIENT_HOLD
			|| global_state == COOP_DLG_CLIENT_RESUME_HOLD;
	}
	/// Runs the timers and handles popups.
	void think() override;
};

}
