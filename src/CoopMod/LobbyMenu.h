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
	Text *_txtTitle, *_txtName, *_txtLatency, *_txtDetails;
	TextList *_lstPlayers;
	ArrowButton *_sortName,  *_sortLatency;
	OptionsOrigin _origin;
	std::vector<playerInfo> _connectedPlayers;
	unsigned int _firstValidRow = 0;
	bool _sortable;
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
	/// Handler for clicking the Name arrow.
	void sortNameClick(Action* action);
	void sortLatencyClick(Action* action);
};

}
