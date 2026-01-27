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
#include "../Interface/ImageButton.h"
#include "../Savegame/EquipmentLayoutItem.h"
#include <map>

namespace OpenXcom
{

class Surface;
class Text;
class TextEdit;
class NumberText;
class InteractiveSurface;

/**
 * Screen that displays the Profile screen.
 */
class Profile : public State
{
  private:
	Surface *_bg;
	Text *_txtName;
	const bool _clientInBattle, _inBattle;
	TextButton *_btnOk;
	ImageButton* _avatar;
	Window *_window;
  public:
	/// Creates the Profile state.
	Profile(bool clientInBattle, bool inBattle);
	/// Cleans up the Profile state.
	~Profile();
  private:
	void buttonOK(Action *);

};

}
