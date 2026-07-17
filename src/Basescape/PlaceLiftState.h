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

namespace OpenXcom
{

class Base;
class BaseView;
class Text;
class TextList;
class Window;
class Globe;
class RuleBaseFacility;

/**
 * Screen shown when the player has to
 * place the access lift of a base.
 */
class PlaceLiftState : public State
{
private:
	Base *_base;
	Globe *_globe;
	BaseView *_view;
	Text *_txtTitle;
	bool _first;
	RuleBaseFacility *_lift;

	std::vector<RuleBaseFacility*> _accessLifts;
	Window *_window;
	Text *_txtHeader;
	TextList *_lstAccessLifts;
public:
	/// Creates the Place Lift state.
	PlaceLiftState(Base *base, Globe *globe, bool first);
	/// Cleans up the Place Lift state.
	~PlaceLiftState();
	/// Handler for clicking the base view.
	void viewClick(Action *action);
	/// Handler for clicking the Access Lifts list.
	void lstAccessLiftsClick(Action *action);
	/// PRD-J07 JOINT: submit the base_new joint_cmd for a SUBSEQUENT base with the
	/// access lift at grid (x,y). Mutates nothing locally; the host creates the base.
	void submitJointNewBase(int x, int y);
	/// Test automation: pick the (front) access lift + submit base_new at (x,y).
	bool harnessPlaceLift(int x, int y);
};

}
