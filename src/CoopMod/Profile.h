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
#include "../Interface/TextButton.h"
#include "../Savegame/EquipmentLayoutItem.h"
#include <map>

namespace OpenXcom
{

class Surface;
class Text;
class TextEdit;
class NumberText;
class InteractiveSurface;
class Inventory;
class SavedBattleGame;
class BattlescapeState;
class BattleUnit;
class BattlescapeButton;
class Base;

/**
 * Screen which displays soldier's inventory.
 */
class Profile : public State
{
  private:
	Surface *_bg, *_soldier;
	Text *_txtItem, *_txtAmmo, *_txtWeight, *_txtTus, *_txtStatLine1, *_txtStatLine2, *_txtStatLine3, *_txtStatLine4;
	Text *_txtName;
	TextEdit *_btnQuickSearch;
	BattlescapeButton *_btnOk, *_btnPrev, *_btnNext, *_btnUnload, *_btnGround, *_btnRank, *_btnArmor;
	BattlescapeButton *_btnCreateTemplate, *_btnApplyTemplate;
	BattlescapeButton *_btnLinks;
	Surface *_selAmmo;
	Inventory *_inv;
	std::vector<EquipmentLayoutItem *> _curInventoryTemplate, _tempInventoryTemplate;
	SavedBattleGame *_battleGame;
	const bool _clientInBattle, _inBattle;
	BattlescapeState *_parent;
	Base *_base;
	std::map<Soldier *, Craft *> _backup;
	bool _resetCustomDeploymentBackup;
	std::string _currentTooltip;
	std::string _currentDamageTooltip;
	int _mouseHoverItemFrame = 0;
	BattleItem *_mouseHoverItem = nullptr;
	BattleItem *_currentDamageTooltipItem = nullptr;
	bool _reloadUnit;
	int _globalLayoutIndex;
	int _prev_key = 0, _key_repeats = 0;
	TextButton *_btnMessage;
	Window *_window;
  public:
	/// Creates the Inventory state.
  Profile(bool tu, Base *base, bool noCraft = false);
	/// Cleans up the Inventory state.
  ~Profile();
	/// Updates all soldier info.
	void setGlobalLayoutIndex(int index, bool armorChanged);
	void init() override;
  private:
	/// Update the visibility and icons for the template buttons.
	void updateTemplateButtons(bool isVisible);
	void buttonOK(Action *);

};

}
