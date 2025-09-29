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
#include <map>
#include <algorithm>
#include <cmath>
#include <yaml-cpp/yaml.h>
#include "../Engine/Screen.h"
#include "../Geoscape/GeoscapeState.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Savegame/BattleUnit.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include "../Basescape/CraftInfoState.h"
#include "../Battlescape/BattlescapeGenerator.h"
#include "../Battlescape/BriefingState.h"
#include "../Engine/Action.h"
#include "../Engine/CrossPlatform.h"
#include "../Engine/Game.h"
#include "../Engine/LocalizedText.h"
#include "../Engine/Logger.h"
#include "../Engine/Options.h"
#include "../Engine/RNG.h"
#include "../Interface/ComboBox.h"
#include "../Interface/Frame.h"
#include "../Interface/Slider.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Interface/TextEdit.h"
#include "../Interface/TextList.h"
#include "../Interface/Window.h"
#include "../Mod/AlienDeployment.h"
#include "../Mod/AlienRace.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleAlienMission.h"
#include "../Mod/RuleCraft.h"
#include "../Mod/RuleGlobe.h"
#include "../Mod/RuleItem.h"
#include "../Mod/RuleTerrain.h"
#include "../Savegame/AlienBase.h"
#include "../Savegame/Base.h"
#include "../Savegame/Craft.h"
#include "../Savegame/ItemContainer.h"
#include "../Savegame/MissionSite.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/Soldier.h"
#include "../Savegame/Ufo.h"

namespace OpenXcom
{

class TextButton;
class TextEdit;
class TextList;
class Window;
class Text;
class ComboBox;
class Slider;
class Frame;
class Craft;
class Screen;
class BattleUnit;
class File;


/**
 * New Battle that displays a list
 * of options to configure a new
 * standalone mission.
 */
class CoopMenu : public State
{
  private:
	TextButton *_btnCancel, *_btnMessage, *_tcpButtonJoin, *_tcpButtonHost, *_btnPVE, *_btnPVP, *_btnPVP2, *_btnPVE2, *_btnChat;
	TextList *_lstSaves;
	TextEdit *_ipAddress, *_playerName;
	Window *_window;
	Text *_txtTitle, *_txtData, *_txtInfo, *_hostPing, *_clientPing;
	std::map<Surface *, bool> _surfaceBackup;
	std::vector<std::string> _missionTypes, _terrainTypes, _alienRaces, _crafts;
	Craft *_craft;
	NewBattleSelectType _selectType;
	bool _isRightClick;
	std::vector<size_t> _filtered;
	static const int TFTD_DEPLOYMENTS = 22;

  public:
	/// Creates the New Battle state.
	CoopMenu();
	/// Cleans up the New Battle state.
	~CoopMenu();
	/// Resets state.
	void init() override;
	/// Handler for clicking the OK button
	void disconnect(Action *action);
	void btnCancelClick(Action *action);
	void joinTCPGame(Action *action);
	void hostTCPGame(Action *action);
	void sendFile();
	void sendFileBase(int state);
	void btnPVEClick(Action *action);
	void btnPVE2Click(Action* action);
	void btnPVPClick(Action *action);
	void btnPVP2Click(Action *action);
	void btnChatClick(Action* action);
	void showGamemode();
	int getGameMode();
	/// Runs the timers and handles popups.
	void think() override;
};

}
