#pragma once
/*
 * Copyright 2010-2026 OpenXcom Developers.
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
#include <string>
#include "../Engine/State.h"
#include "OptionsBaseState.h" // OptionsOrigin
#include "../Savegame/Upgrade/SaveUpgrade.h" // DetectedSchema

namespace OpenXcom
{

class TextButton;
class Window;
class Text;

/**
 * The AmbiguousBuild load-gate dialog: the save carries only WEAK co-op traces
 * (keys the old co-op-capable build wrote for every save), so it might be a
 * legacy co-op campaign or an ordinary solo save. Only the player knows. Offers
 * three choices instead of guessing:
 *   - "Load as solo campaign" -> a normal load that bypasses the gate (the save
 *     re-stamps itself schema-current on its next save).
 *   - "Upgrade as co-op save" -> the exact legacy dual upgrade route (client save
 *     picker -> runner), by confirming this was a co-op campaign.
 *   - Cancel -> pop back to the invoker, nothing written.
 * Modelled on SaveUpgradeDialogState; self-themes over both palettes.
 */
class SaveUpgradeAmbiguousState : public State
{
private:
	OptionsOrigin _origin;
	std::string _filename;
	SaveUpgrade::DetectedSchema _detected;
	Window *_window;
	Text *_txtTitle, *_txtText;
	TextButton *_btnSolo, *_btnCoop, *_btnCancel;

	std::string displayName() const;
public:
	/// Creates the ambiguous-build choice dialog.
	SaveUpgradeAmbiguousState(OptionsOrigin origin, const std::string& filename, const SaveUpgrade::DetectedSchema& detected);
	/// Cleans up the state.
	~SaveUpgradeAmbiguousState();
	/// "Load as solo campaign": normal load, bypassing the schema gate.
	void btnSoloClick(Action* action);
	/// "Upgrade as co-op save": route through the legacy dual upgrade flow.
	void btnCoopClick(Action* action);
	/// Cancels back to the previous screen (nothing written).
	void btnCancelClick(Action* action);
};

}
