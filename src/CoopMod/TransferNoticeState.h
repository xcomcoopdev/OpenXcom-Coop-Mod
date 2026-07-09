#pragma once
/*
 * Copyright 2010-2016 OpenXcom Developers.
 * Copyright 2023-2026 XComCoopTeam (https://www.moddb.com/mods/openxcom-coop-mod)
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
#include <string>

namespace OpenXcom
{

class Window;
class Text;
class TextButton;

/**
 * Small co-op notification popup ("X transferred ownership of Y to you...").
 * Adopts the palette of the state it is pushed over, so it can appear on any
 * screen (geoscape, basescape, peer-base view) without a palette flash. As a
 * side effect, closing it re-inits the state below, refreshing soldier lists.
 */
class TransferNoticeState : public State
{
private:
	Window *_window;
	Text *_txtMessage;
	TextButton *_btnOk;
	std::string _category;

public:
	TransferNoticeState(const std::string &message);
	void btnOkClick(Action *action);
	/// Interface category the widgets were themed with (test introspection).
	const std::string &getCategory() const { return _category; }
};

}
