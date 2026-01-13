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
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/LocalizedText.h"
#include "../Engine/Options.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Interface/TextList.h"
#include "../Interface/Window.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleInterface.h"
#include "../CoopMod/OptionsMultiplayer.h"
#include <algorithm>
#include <sstream>

namespace OpenXcom
{

/**
 * Initializes all the elements in the Advanced Options window.
 * @param game Pointer to the core game.
 * @param origin Game section that originated this state.
 */
OptionsMultiplayerState::OptionsMultiplayerState(OptionsOrigin origin) : OptionsBaseState(origin), _selected(-1), _selKey(0)
{
	setCategory(_btnMultiplayer);

	// Create objects
	_btnOTHER = new TextButton(70, 16, 242, 8);
	_lstOptions = new TextList(200, 120, 94, 26);

	_owner = _btnOTHER;

	_isTFTD = false;
	for (const auto& pair : Options::mods)
	{
		if (pair.second)
		{
			if (pair.first == "xcom2")
			{
				_isTFTD = true;
				break;
			}
		}
	}

	add(_btnOTHER, "button", "advancedMenu");

	if (origin != OPT_BATTLESCAPE)
	{
		_greyedOutColor = _game->getMod()->getInterface("advancedMenu")->getElement("disabledUserOption")->color;
		add(_lstOptions, "optionLists", "advancedMenu");
	}
	else
	{
		_greyedOutColor = _game->getMod()->getInterface("battlescape")->getElement("disabledUserOption")->color;
		add(_lstOptions, "optionLists", "battlescape");
	}

	centerAllSurfaces();

	_btnOTHER->setText(tr("STR_ENGINE_OTHER")); // rename in your fork
	_btnOTHER->setGroup(&_owner);
	_btnOTHER->onMousePress((ActionHandler)&OptionsMultiplayerState::btnGroupPress, SDL_BUTTON_LEFT);
	_btnOTHER->setVisible(false); // enable in your fork

	// how much room do we need for YES/NO
	Text text = Text(100, 9, 0, 0);
	text.initText(_game->getMod()->getFont("FONT_BIG"), _game->getMod()->getFont("FONT_SMALL"), _game->getLanguage());
	text.setText(tr("STR_YES"));
	int yes = text.getTextWidth();
	text.setText(tr("STR_NO"));
	int no = text.getTextWidth();

	int rightcol = std::max(yes, no) + 2;
	int leftcol = _lstOptions->getWidth() - rightcol;

	// Set up objects 2
	_lstOptions->setAlign(ALIGN_RIGHT, 1);
	_lstOptions->setColumns(2, leftcol, rightcol);
	_lstOptions->setWordWrap(true);
	_lstOptions->setSelectable(true);
	_lstOptions->setBackground(_window);
	_lstOptions->onMouseClick((ActionHandler)&OptionsMultiplayerState::lstOptionsClick, 0);
	_lstOptions->onMouseOver((ActionHandler)&OptionsMultiplayerState::lstOptionsMouseOver);
	_lstOptions->onMouseOut((ActionHandler)&OptionsMultiplayerState::lstOptionsMouseOut);

	_lstOptions->onKeyboardPress((ActionHandler)&OptionsMultiplayerState::lstControlsKeyPress);

	_lstOptions->setFocus(true);
	_lstOptions->setTooltip("STR_CONTROLS_DESC");
	_lstOptions->onMouseIn((ActionHandler)&OptionsMultiplayerState::txtTooltipIn);
	_lstOptions->onMouseOut((ActionHandler)&OptionsMultiplayerState::txtTooltipOut);

	_colorGroup = _lstOptions->getSecondaryColor();

	_colorSel = _lstOptions->getScrollbarColor();
	_colorNormal = _lstOptions->getColor();

	for (const auto& optionInfo : Options::getOptionInfo())
	{
	
		if (optionInfo.category() == "STR_GENERAL")
		{
			_settingsControls[optionInfo.owner()].push_back(optionInfo);
		}
		else if (optionInfo.category() == "STR_GEOSCAPE")
		{
			_settingsGeo[optionInfo.owner()].push_back(optionInfo);
		}
		else if (optionInfo.category() == "STR_BASESCAPE")
		{				
			_settingsBase[optionInfo.owner()].push_back(optionInfo);
		}
		else if (optionInfo.category() == "STR_BATTLESCAPE")
		{
			_settingsBattle[optionInfo.owner()].push_back(optionInfo);
		}
		else if (optionInfo.category() == "STR_AI")
		{
			_settingsGeneral[optionInfo.owner()].push_back(optionInfo);
		}

	}
}

/**
 *
 */
OptionsMultiplayerState::~OptionsMultiplayerState()
{
}

/**
 * Refreshes the UI.
 */
void OptionsMultiplayerState::init()
{
	OptionsBaseState::init();

	updateList();
}

/**
 * Adds a bunch of controls to the list.
 * @param keys List of controls.
 */
void OptionsMultiplayerState::addControls(const std::vector<OptionInfo>& keys)
{
	for (const auto& optionInfo : keys)
	{

		if (optionInfo.type() == OPTION_KEY)
		{
			std::string name = tr(optionInfo.description());
			SDLKey* key = optionInfo.asKey();
			std::string keyName = ucWords(SDL_GetKeyName(*key));
			if (*key == SDLK_UNKNOWN)
				keyName = "";
			_lstOptions->addRow(2, name.c_str(), keyName.c_str());
		}

	}
}

/**
 * Uppercases all the words in a string.
 * @param str Source string.
 * @return Destination string.
 */
std::string OptionsMultiplayerState::ucWords(std::string str)
{
	if (!str.empty())
	{
		str[0] = toupper(str[0]);
	}
	for (size_t i = str.find_first_of(' '); i != std::string::npos; i = str.find_first_of(' ', i + 1))
	{
		if (str.length() > i + 1)
			str[i + 1] = toupper(str[i + 1]);
		else
			break;
	}
	return str;
}

/**
 * Fills the settings list based on category.
 */
void OptionsMultiplayerState::updateList()
{
	OptionOwner idx = OPTION_OTHER;

	_offsetControlsMin = -1;
	_offsetControlsMax = -1;
	_offsetGeneralMin = -1;
	_offsetGeneralMax = -1;
	_offsetGeoMin = -1;
	_offsetGeoMax = -1;
	_offsetBaseMin = -1;
	_offsetBaseMax = -1;
	_offsetBattleMin = -1;
	_offsetBattleMax = -1;
	_offsetAIMin = -1;
	_offsetAIMax = -1;

	_lstOptions->clearList();

	int row = -1;

	if (_settingsControls[idx].size() > 0)
	{
		_lstOptions->addRow(2, tr("Controls").c_str(), "");
		row++;
		_offsetGeneralMin = row;
		_lstOptions->setCellColor(_offsetGeneralMin, 0, _colorGroup);
		addControls(_settingsControls[idx]);
		row += _settingsControls[idx].size();
		_offsetGeneralMax = row;
	}

	if (_settingsGeneral[idx].size() > 0)
	{
		if (row > -1)
		{
			_lstOptions->addRow(2, "", "");
			row++;
		}
		_lstOptions->addRow(2, tr("STR_GENERAL").c_str(), "");
		row++;
		_offsetGeneralMin = row;
		_lstOptions->setCellColor(_offsetGeneralMin, 0, _colorGroup);
		addSettings(_settingsGeneral[idx]);
		row += _settingsGeneral[idx].size();
		_offsetGeneralMax = row;
	}

	if (_settingsGeo[idx].size() > 0)
	{
		if (row > -1)
		{
			_lstOptions->addRow(2, "", "");
			row++;
		}
		_lstOptions->addRow(2, tr("STR_GEOSCAPE").c_str(), "");
		row++;
		_offsetGeoMin = row;
		_lstOptions->setCellColor(_offsetGeoMin, 0, _colorGroup);
		addSettings(_settingsGeo[idx]);
		row += _settingsGeo[idx].size();
		_offsetGeoMax = row;
	}

	if (_settingsBase[idx].size() > 0)
	{
		if (row > -1)
		{
			_lstOptions->addRow(2, "", "");
			row++;
		}
		_lstOptions->addRow(2, tr("STR_BASESCAPE").c_str(), "");
		row++;
		_offsetBaseMin = row;
		_lstOptions->setCellColor(_offsetBaseMin, 0, _colorGroup);
		addSettings(_settingsBase[idx]);
		row += _settingsBase[idx].size();
		_offsetBaseMax = row;
	}

	if (_settingsBattle[idx].size() > 0)
	{
		if (row > -1)
		{
			_lstOptions->addRow(2, "", "");
			row++;
		}
		_lstOptions->addRow(2, tr("STR_BATTLESCAPE").c_str(), "");
		row++;
		_offsetBattleMin = row;
		_lstOptions->setCellColor(_offsetBattleMin, 0, _colorGroup);
		addSettings(_settingsBattle[idx]);
		row += _settingsBattle[idx].size();
		_offsetBattleMax = row;
	}

}

/**
 * Change selected control.
 * @param action Pointer to an action.
 */
void OptionsMultiplayerState::lstControlsKeyPress(Action* action)
{
	if (_selected != -1)
	{
		SDLKey key = action->getDetails()->key.keysym.sym;
		if (key != 0 &&
			key != SDLK_LSHIFT && key != SDLK_LALT && key != SDLK_LCTRL &&
			key != SDLK_RSHIFT && key != SDLK_RALT && key != SDLK_RCTRL)
		{
			*_selKey->asKey() = key;
			std::string name = ucWords(SDL_GetKeyName(*_selKey->asKey()));
			_lstOptions->setCellText(_selected, 1, name);
		}
		_lstOptions->setCellColor(_selected, 0, _colorNormal);
		_lstOptions->setCellColor(_selected, 1, _colorNormal);
		_selected = -1;
		_selKey = 0;
	}
}


/**
 * Adds a bunch of settings to the list.
 * @param settings List of settings.
 */
void OptionsMultiplayerState::addSettings(const std::vector<OptionInfo>& settings)
{
	auto& fixeduserOptions = _game->getMod()->getFixedUserOptions();
	for (const auto& optionInfo : settings)
	{
		std::string name = tr(optionInfo.description());
		std::string value;
		if (optionInfo.type() == OPTION_BOOL)
		{
			value = *optionInfo.asBool() ? tr("STR_YES") : tr("STR_NO");
		}
		else if (optionInfo.type() == OPTION_INT)
		{
			std::ostringstream ss;
			ss << *optionInfo.asInt();
			value = ss.str();
		}
		_lstOptions->addRow(2, name.c_str(), value.c_str());
		// grey out fixed options
		auto search = fixeduserOptions.find(optionInfo.id());
		if (search != fixeduserOptions.end())
		{
			_lstOptions->setRowColor(_lstOptions->getLastRowIndex(), _greyedOutColor);
		}
	}
}

/**
 * Gets the currently selected setting.
 * @param sel Selected row.
 * @return Pointer to option, NULL if none selected.
 */
OptionInfo* OptionsMultiplayerState::getSetting(size_t sel)
{
	int selInt = sel;
	OptionOwner idx = OPTION_OTHER;

	if (selInt > _offsetControlsMin && selInt <= _offsetControlsMax)
	{
		return &_settingsControls[idx][selInt - 1 - _offsetControlsMin];
	}
	else if (selInt > _offsetGeoMin && selInt <= _offsetGeoMax)
	{
		return &_settingsGeo[idx][selInt - 1 - _offsetGeoMin];
	}
	else if (selInt > _offsetBaseMin && selInt <= _offsetBaseMax)
	{
		return &_settingsBase[idx][selInt - 1 - _offsetBaseMin];
	}
	else if (selInt > _offsetBattleMin && selInt <= _offsetBattleMax)
	{
		return &_settingsBattle[idx][selInt - 1 - _offsetBattleMin];
	}
	else if (selInt > _offsetGeneralMin && selInt <= _offsetGeneralMax)
	{
		return &_settingsGeneral[idx][selInt - 1 - _offsetGeneralMin];
	}
	else
	{
		return 0;
	}
}

/**
 * Changes the clicked setting.
 * @param action Pointer to an action.
 */
void OptionsMultiplayerState::lstOptionsClick(Action* action)
{
	Uint8 button = action->getDetails()->button.button;
	if (button != SDL_BUTTON_LEFT && button != SDL_BUTTON_RIGHT)
	{
		return;
	}
	size_t sel = _lstOptions->getSelectedRow();
	OptionInfo* setting = getSetting(sel);
	if (!setting)
		return;

	// greyed out options are fixed, cannot be changed by the user
	auto& fixeduserOptions = _game->getMod()->getFixedUserOptions();
	auto it = fixeduserOptions.find(setting->id());
	if (it != fixeduserOptions.end())
	{
		return;
	}

	std::string settingText;
	if (setting->type() == OPTION_BOOL)
	{
		bool* b = setting->asBool();
		*b = !*b;
		settingText = *b ? tr("STR_YES") : tr("STR_NO");
		if (b == &Options::lazyLoadResources && !*b)
		{
			Options::reload = true; // reload when turning lazy loading off
		}
	}
	else if (setting->type() == OPTION_INT) // integer variables will need special handling
	{
		int* i = setting->asInt();

		int increment = (button == SDL_BUTTON_LEFT) ? 1 : -1; // left-click increases, right-click decreases
		if (i == &Options::changeValueByMouseWheel || i == &Options::FPS || i == &Options::FPSInactive || i == &Options::oxceWoundedDefendBaseIf)
		{
			increment *= 10;
		}
		else if (i == &Options::oxceResearchScrollSpeedWithCtrl || i == &Options::oxceManufactureScrollSpeedWithCtrl)
		{
			increment *= 5;
		}
		else if (i == &Options::oxceInterceptTableSize)
		{
			increment *= 4;
		}
		*i += increment;

		int min = 0, max = 0;
		if (i == &Options::battleExplosionHeight)
		{
			min = 0;
			max = 3;
		}
		else if (i == &Options::changeValueByMouseWheel)
		{
			min = 0;
			max = 100;
		}
		else if (i == &Options::FPS)
		{
			min = 0;
			max = 120;
		}
		else if (i == &Options::FPSInactive)
		{
			min = 10;
			max = 120;
		}
		else if (i == &Options::mousewheelSpeed)
		{
			min = 1;
			max = 7;
		}
		else if (i == &Options::autosaveFrequency)
		{
			min = 1;
			max = 5;
		}
		else if (i == &Options::oxceGeoAutosaveFrequency)
		{
			min = 0;
			max = 10;
		}
		else if (i == &Options::autosaveSlots || i == &Options::oxceGeoAutosaveSlots || i == &Options::oxceResearchScrollSpeed || i == &Options::oxceManufactureScrollSpeed)
		{
			min = 1;
			max = 10;
		}
		else if (i == &Options::oxceInterceptGuiMaintenanceTime || i == &Options::oxceShowETAMode || i == &Options::oxceShowAccuracyOnCrosshair || i == &Options::oxceCrashedOrLanded)
		{
			min = 0;
			max = 2;
		}
		else if (i == &Options::oxceInterceptTableSize)
		{
			min = 8;
			max = 80;
		}
		else if (i == &Options::oxceWoundedDefendBaseIf)
		{
			min = 0;
			max = 100;
		}
		else if (i == &Options::oxceResearchScrollSpeedWithCtrl || i == &Options::oxceManufactureScrollSpeedWithCtrl)
		{
			min = 5;
			max = 50;
		}
		else if (i == &Options::oxceAutoNightVisionThreshold)
		{
			min = 0;
			max = 15;
		}
		else if (i == &Options::oxceNightVisionColor)
		{
			// UFO: 1-15, TFTD: 2-16 except 8 and 10
			if (_isTFTD && ((*i) == 8 || (*i) == 10))
			{
				*i += increment;
			}
			min = _isTFTD ? 2 : 1;
			max = _isTFTD ? 16 : 15;
		}

		if (*i < min)
		{
			*i = max;
		}
		else if (*i > max)
		{
			*i = min;
		}

		std::ostringstream ss;
		ss << *i;
		settingText = ss.str();
	}
	else if (setting->type() == OPTION_KEY)
	{

		if (_selected != -1)
		{
			unsigned int selected = _selected;
			_lstOptions->setCellColor(_selected, 0, _colorNormal);
			_lstOptions->setCellColor(_selected, 1, _colorNormal);
			_selected = -1;
			_selKey = 0;
			if (selected == _lstOptions->getSelectedRow())
				return;
		}
		_selected = _lstOptions->getSelectedRow();
		_selKey = getSetting(_selected);
		if (!_selKey)
		{
			_selected = -1;
			return;
		}

		if (action->getDetails()->button.button == SDL_BUTTON_LEFT)
		{
			_lstOptions->setCellColor(_selected, 0, _colorSel);
			_lstOptions->setCellColor(_selected, 1, _colorSel);
		}
		else if (action->getDetails()->button.button == SDL_BUTTON_RIGHT)
		{
			_lstOptions->setCellText(_selected, 1, "");
			*_selKey->asKey() = SDLK_UNKNOWN;
			_selected = -1;
			_selKey = 0;
		}

	}

	_lstOptions->setCellText(sel, 1, settingText);
}

void OptionsMultiplayerState::lstOptionsMouseOver(Action*)
{
	size_t sel = _lstOptions->getSelectedRow();
	OptionInfo* setting = getSetting(sel);
	std::string desc;
	if (setting)
	{
		desc = tr(setting->description() + "_DESC");
	}
	_txtTooltip->setText(desc);
}

void OptionsMultiplayerState::lstOptionsMouseOut(Action*)
{
	_txtTooltip->setText("");
}

void OptionsMultiplayerState::btnGroupPress(Action*)
{
	updateList();
}

}
