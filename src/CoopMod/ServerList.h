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
class ServerList : public State
{
protected:
	TextButton *_btnHost, *_btnDirectConnect, *_btnAddServer, *_btnRefresh, *_btnCancel, *_btnFilter;
	TextEdit *_search, *_playername;
	Window *_window;
	Text *_txtTitle, *_txtName, *_txtPlayers, *_txtRegion, *_txtJoin, *_txtPasswordRequired;
	TextList *_lstServers;
	ArrowButton *_sortName, *_sortPlayers, *_sortRegion, *_sortPassword;
	OptionsOrigin _origin;
	std::vector<ServerInfo> _servers;
	// selected
	std::string selectedNetworkProtocol = "NETWORK: ANY";
	std::string selectedRegion = "ANY REGION";
	std::string selectedCampaign = "Any game mode";
	std::string selectedPassword = "With or without password";
	std::string selectedModCompatibility = "All mods";
	std::string selectedManualServers = "ANY SERVER";
	unsigned int _firstValidRow = 0;
	bool _sortable;
	void updateArrows();
	void updateServerList();
	void loadServersFromJson();
	void loadFilters();
	bool removeManuallyAddedServerFromFile();
	void savePlayerNameToIpAddressFile(std::string playerName);
	bool isAllowedBySearch(std::string serverName);
	bool isAllowedByFilters(std::string region, bool passwordRequired, bool isUDP, std::string modHash, bool added, bool isCampaign);
	bool parseUdpPort(const std::string& text, uint16_t& outPort);
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
	void btnSearchClick(Action* action);
	void btnRefreshClick(Action* action);
	void btnHostClick(Action* action);
	void btnDirectConnectClick(Action* action);
	void btnAddServerClick(Action* action);
	/// Handler for moving the mouse over a list item.
	void lstServerMouseOver(Action *action);
	/// Handler for moving the mouse outside the list borders.
	void lstServerMouseOut(Action *action);
	/// Handler for clicking the Server list.
	virtual void lstServerPress(Action *action);
	/// Handler for clicking the Name arrow.
	void sortNameClick(Action *action);
	/// Handler for clicking the Date arrow.
	void sortPlayersClick(Action *action);
	void sortRegionClick(Action* action);
	void sortPasswordClick(Action* action);
	void edtPlayerNameChange(Action* action);
	void edtSearchChange(Action* action);
	/// disables the sort buttons.
	void disableSort();
	/// Runs the timers and handles popups.
	void think() override;
};

}
