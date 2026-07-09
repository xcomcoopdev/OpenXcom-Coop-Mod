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
#include "ComboBox.h"

namespace OpenXcom
{

/**
 * A ComboBox where individual options can be disabled: disabled options are
 * drawn in a dimmed color and cannot be picked by the user. Programmatic
 * selection (forceSelect) may still land on a disabled option, so a disabled
 * item can be shown as the current selection (e.g. an offline server).
 */
class DisableableComboBox : public ComboBox
{
private:
	std::vector<bool> _enabled;
	Uint8 _disabledColor;
public:
	/// Creates a disableable combo box with the specified size and position.
	DisableableComboBox(State *state, int width, int height, int x = 0, int y = 0, bool popupAboveButton = false);
	/// Cleans up the combo box.
	~DisableableComboBox();
	/// Sets the color used to dim disabled options (0 = leave default color).
	void setDisabledColor(Uint8 color);
	/// Sets the label color of the (closed) combobox button, leaving the button
	/// face color unchanged. Used to dim the text when the current selection is
	/// disabled/offline.
	void setButtonTextColor(Uint8 color);
	/// Sets the list of options together with a per-option enabled mask.
	void setOptions(const std::vector<std::string> &options, const std::vector<bool> &enabled, bool translate = false);
	/// Returns whether the option at the given index is enabled (selectable).
	bool isEnabled(size_t idx) const;
	/// Selects an option regardless of its enabled state (bypasses the guard).
	void forceSelect(size_t idx);
	/// Sets the selected option, ignoring the request if that option is disabled.
	void setSelected(size_t sel) override;
};

}
