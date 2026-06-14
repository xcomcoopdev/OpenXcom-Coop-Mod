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
#include "../Engine/Screen.h"
#include "../Geoscape/GeoscapeState.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Savegame/BattleUnit.h"
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
class AddServerMenu : public State
{
  private:
	TextButton *_btnCancel, *_btnAdd;
	TextEdit *_ipAddress, *_serverName, *_port;
	Window *_window;
	Text *_txtTitle, *_txtInfo;
	std::map<Surface *, bool> _surfaceBackup;
	Craft *_craft;
	std::vector<std::string> _campaignTypes, _regionTypes;
	NewBattleSelectType _selectType;
	ComboBox *_cbxNetworkProtocol, *_cbxRegions, *_cbxCampaign;
	std::vector<std::string> _networkProtocolTypes;
	bool _isRightClick;
	std::string selectedRegion = "NORTH AMERICA";
	bool isCampaign = true;
	std::vector<size_t> _filtered;
	static const int TFTD_DEPLOYMENTS = 22;
	bool isUDP = false;
	bool parseUdpPort(const std::string& text, uint16_t& outPort);
  public:
	/// Creates the New Battle state.
	AddServerMenu();
	/// Cleans up the New Battle state.
	~AddServerMenu();
	/// Resets state.
	void init() override;
	void btnCancelClick(Action *action);
	void btnCAddServerClick(Action* action);
	void cbxRegionChange(Action* action);
	void cbxRCampaignChange(Action* action);
	void cbxNetworkProtocolChange(Action* action);
};

}
