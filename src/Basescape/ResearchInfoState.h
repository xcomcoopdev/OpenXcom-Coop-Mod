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

class TextButton;
class Window;
class Text;
class Base;
class RuleResearch;
class ResearchProject;
class ArrowButton;
class Timer;
class InteractiveSurface;

/**
 * Window which allows changing of the number of assigned scientist to a project.
 */
class ResearchInfoState : public State
{
private:
	Base *_base;
	TextButton *_btnOk;
	TextButton *_btnCancel;
	ArrowButton *_btnMore, *_btnLess;
	Window *_window;
	Text *_txtTitle, *_txtAvailableScientist, *_txtAvailableSpace, *_txtAllocatedScientist, *_txtMore, *_txtLess;
	void setAssignedScientist();
	ResearchProject *_project;
	RuleResearch *_rule;
	void buildUi();
	Timer *_timerMore, *_timerLess;
	InteractiveSurface *_surfaceScientists;
	// PRD-J06 (JOINT): true in a JOINT campaign. The screen edits the shared world
	// live (reusing vanilla's capping), then on OK/Cancel it UNDOES those edits and
	// submits a res_start/res_alloc/res_cancel joint_cmd - the host-authoritative
	// world settles via joint_apply. _jointOrigAssigned is the pre-edit scientist
	// count of an EXISTING project, used to reverse the local edits before submit.
	bool _joint;
	int _jointOrigAssigned;
public:
	/// Creates the ResearchProject state.
	ResearchInfoState(Base *base, RuleResearch *rule);
	ResearchInfoState(Base *base, ResearchProject *project);
	/// Cleans up the ResearchInfo state
	~ResearchInfoState();
	/// Handler for clicking the OK button.
	void btnOkClick(Action *action);
	/// Handler for clicking the Cancel button.
	void btnCancelClick(Action *action);
	/// Function called every time the _timerMore timer is triggered.
	void more();
	/// Add given number of scientists to the project if possible
	void moreByValue(int change);
	/// Function called every time the _timerLess timer is triggered.
	void less();
	/// Remove the given number of scientists from the project if possible
	void lessByValue(int change);
	/// Handler for using the mouse wheel.
	void handleWheel(Action *action);
	/// Handler for pressing the More button.
	void morePress(Action *action);
	/// Handler for releasing the More button.
	void moreRelease(Action *action);
	/// Handler for clicking the More button.
	void moreClick(Action *action);
	/// Handler for pressing the Less button.
	void lessPress(Action *action);
	/// Handler for releasing the Less button.
	void lessRelease(Action *action);
	/// Handler for clicking the Less button.
	void lessClick(Action *action);
	/// Runs state functionality every cycle(used to update the timer).
	void think() override;
	/// Test harness (JOINT): allocate N scientists via the vanilla arrow path and
	/// confirm (btnOkClick) - exercises the real "start project" submit path.
	bool harnessStart(int scientists);
};

}
