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
class ServerList : public State
{
protected:
	TextButton *_btnHost, *_btnDirectConnect, *_btnAddServer, *_btnFilter, *_btnCancel;
	Window *_window;
	ToggleTextButton* _btnJoin;
	Text *_txtTitle, *_txtName, *_txtPlayers, *_txtLatency, *_txtJoin, *_txtDetails;
	TextList *_lstServers;
	ArrowButton *_sortName, *_sortPlayers,  *_sortLatency;
	OptionsOrigin _origin;
	std::vector<ServerInfo> _servers;
	unsigned int _firstValidRow = 0;
	bool _sortable;
	void updateArrows();
public:
	/// Creates the Saved Game state.
	ServerList();
	/// Cleans up the Saved Game state.
	virtual ~ServerList();
	/// Sets up the servers list.
	void init() override;
	/// Sorts the server list.
	void sortList(serverSort sort);
	/// Updates the server list.
	virtual void updateList();
	/// Handler for clicking the Cancel button.
	void btnCancelClick(Action *action);
	void btnFilterClick(Action* action);
	void btnHostClick(Action* action);
	void btnDirectConnectClick(Action* action);
	/// Handler for moving the mouse over a list item.
	void lstSavesMouseOver(Action *action);
	/// Handler for moving the mouse outside the list borders.
	void lstSavesMouseOut(Action *action);
	/// Handler for clicking the Saves list.
	virtual void lstSavesPress(Action *action);
	/// Handler for clicking the Name arrow.
	void sortNameClick(Action *action);
	/// Handler for clicking the Date arrow.
	void sortPlayersClick(Action *action);
	void sortLatencyClick(Action* action);
	/// disables the sort buttons.
	void disableSort();
};

}
