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
class ModCheckMenu : public State
{
protected:
	TextButton *_btnCancel;
	Window *_window;
	Text *_txtTitle, *_txtModName, *_txtModRequired;
	TextList *_lstMods;
	ArrowButton *_sortModName, *_sortModRequired;
	OptionsOrigin _origin;
	std::vector<ModInfoCoop> _mods;
	std::string _modHash;
	unsigned int _firstValidRow = 0;
	bool _sortable;
	void updateArrows();
	void updateModList();
  public:
	/// Creates the Saved Game state.
	ModCheckMenu(std::string modHash);
	/// Cleans up the Saved Game state.
	virtual ~ModCheckMenu();
	/// Sets up the mod list.
	void init() override;
	/// Sorts the server list.
	void sortList(modSort sort);
	/// Updates the server list.
	virtual void updateList();
	/// Handler for clicking the Cancel button.
	void btnCancelClick(Action *action);
	/// Handler for clicking the Name arrow.
	void sortModNameClick(Action *action);
	void sortModRequiredClick(Action* action);
	/// disables the sort buttons.
	void disableSort();
};

}
