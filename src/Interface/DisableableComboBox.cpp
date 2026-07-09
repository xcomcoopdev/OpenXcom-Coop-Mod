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
#include "DisableableComboBox.h"
#include "TextList.h"
#include "TextButton.h"

namespace OpenXcom
{

/**
 * Sets up the disableable combo box with the specified size and position.
 * @param state Pointer to state the combo box belongs to.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param x X position in pixels.
 * @param y Y position in pixels.
 * @param popupAboveButton Popup direction.
 */
DisableableComboBox::DisableableComboBox(State *state, int width, int height, int x, int y, bool popupAboveButton)
	: ComboBox(state, width, height, x, y, popupAboveButton), _disabledColor(0)
{
}

/**
 *
 */
DisableableComboBox::~DisableableComboBox()
{
}

/**
 * Sets the palette color used to dim disabled options. 0 leaves the default color.
 * @param color Palette index.
 */
void DisableableComboBox::setDisabledColor(Uint8 color)
{
	_disabledColor = color;
}

/**
 * Sets the label color of the closed combobox button, leaving the button face
 * color unchanged.
 * @param color Palette index.
 */
void DisableableComboBox::setButtonTextColor(Uint8 color)
{
	_button->setTextColor(color);
}

/**
 * Changes the list of available options together with a per-option enabled mask.
 * Disabled options are dimmed and cannot be selected by the user.
 * @param options List of strings.
 * @param enabled Per-option enabled flags (missing entries default to enabled).
 * @param translate True for a list of string IDs, false for raw strings.
 */
void DisableableComboBox::setOptions(const std::vector<std::string> &options, const std::vector<bool> &enabled, bool translate)
{
	// Store the mask first so the base class's internal setSelected() call
	// respects the new enabled state.
	_enabled.assign(options.size(), true);
	for (size_t i = 0; i < options.size() && i < enabled.size(); ++i)
		_enabled[i] = enabled[i];

	ComboBox::setOptions(options, translate);

	if (_disabledColor != 0)
	{
		for (size_t i = 0; i < _enabled.size(); ++i)
		{
			if (!_enabled[i])
				_list->setRowColor(i, _disabledColor);
		}
	}
}

/**
 * @param idx Option index.
 * @return True if the option is enabled/selectable (unknown index defaults to true).
 */
bool DisableableComboBox::isEnabled(size_t idx) const
{
	if (idx < _enabled.size())
		return _enabled[idx];
	return true;
}

/**
 * Selects an option regardless of its enabled state. Used for programmatic
 * selection (e.g. restoring a saved, currently-offline server).
 * @param idx Option index.
 */
void DisableableComboBox::forceSelect(size_t idx)
{
	ComboBox::setSelected(idx);
}

/**
 * Sets the selected option, but ignores the request when that option is
 * disabled. The click-commit path in TextList routes through this virtual, so a
 * user click on a disabled row is silently rejected (selection is unchanged).
 * @param sel Selected row.
 */
void DisableableComboBox::setSelected(size_t sel)
{
	if (sel < _enabled.size() && !_enabled[sel])
		return;
	ComboBox::setSelected(sel);
}

}
