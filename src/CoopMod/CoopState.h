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
#include "../Menu/OptionsBaseState.h"
#include "../Engine/Screen.h"
#include "../Geoscape/Globe.h"

namespace OpenXcom
{

class TextButton;
class Window;
class Text;
class GeoscapeState;

/**
 * Options window shown for loading/saving/quitting the game.
 * Not to be confused with the Game Options window
 * for changing game settings during runtime.
 */
class CoopState : public State
{
  private:
	OptionsOrigin _origin;
	Window *_window;
	Text *_txtTitle;
	TextButton *_btnMessage, *_btnBack, *_btnYes;

  public:
	/// Creates the Pause state.
	CoopState(int state);
	/// Cleans up the Pause state.
	~CoopState();
	void loadCoop(Action *);
	void previous(Action *);
	void btnYesClick(Action *);
	void loadWorld();
	void setGlobe(Globe *globe);
	void setBaseName(std::string name);
	/// Runs the timers and handles popups.
	void think() override;
};

}
