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
 * along with OpenXcom.  If not, see <http:///www.gnu.org/licenses/>.
 */
#include "../Engine/State.h"
#include <list>
#include <map>

namespace OpenXcom
{

class Surface;
class Globe;
class TextButton;
class InteractiveSurface;
class Text;
class ComboBox;
class Timer;
class DogfightState;
class Craft;
class Ufo;
class MissionSite;
class Base;
class RuleMissionScript;
class RuleEvent;
class AlienBase;
class Texture;

/**
 * Geoscape screen which shows an overview of
 * the world and lets the player manage the game.
 */
class GeoscapeState : public State
{
private:
	Surface *_bg, *_sideLine, *_sidebar;
	Globe *_globe;
	TextButton *_btnIntercept, *_btnBases, *_btnGraphs, *_btnUfopaedia, *_btnOptions, *_btnFunding;
	TextButton *_timeSpeed;
	TextButton *_btn5Secs, *_btn1Min, *_btn5Mins, *_btn30Mins, *_btn1Hour, *_btn1Day;
	TextButton *_sideTop, *_sideBottom;
	InteractiveSurface *_btnRotateLeft, *_btnRotateRight, *_btnRotateUp, *_btnRotateDown, *_btnZoomIn, *_btnZoomOut;
	Text *_txtFunds, *_txtHour, *_txtHourSep, *_txtMin, *_txtMinSep, *_txtSec, *_txtWeekday, *_txtDay, *_txtMonth, *_txtYear;
	// coop: ally-location markers ('+' per other player). _peerSpeedMarker is indexed
	// 0..5 = 5 Secs / 1 Min / 5 Mins / 30 Mins / 1 Hour / 1 Day (shown while the peer is
	// on the geoscape). _peerScreenMarker is indexed 0..5 = Intercept / Bases / Graphs /
	// Ufopaedia / Options / Funding (shown while the peer is looking at that screen).
	Text *_peerSpeedMarker[6];
	Text *_peerScreenMarker[6];
	Timer *_gameTimer, *_zoomInEffectTimer, *_zoomOutEffectTimer, *_dogfightStartTimer, *_dogfightTimer;
	bool _pause, _zoomInEffectDone, _zoomOutEffectDone;
	Text *_txtDebug;
	ComboBox *_cbxRegion, *_cbxZone, *_cbxArea, *_cbxCountry;
	Text *_txtSlacking;
	Text *_txtTraining;
	std::list<State*> _popups;
	std::list<DogfightState*> _dogfights, _dogfightsToBeStarted;
	std::vector<Craft*> _activeCrafts;
	size_t _minimizedDogfights;
	int _slowdownCounter;

	// PRD-J10 landing broker (HOST only). A craft another seat commanded reached a
	// landable target: instead of popping ConfirmLandingState here, we ask THAT
	// seat and park the craft until it answers. This map is both the "already
	// asked, do not ask again every tick" guard and the stash of the world data the
	// dialog needs, which only the host's globe can compute.
	struct JointLandingPrompt { Texture* missionTexture; Texture* globeTexture; int shade; };
	std::map<Craft*, JointLandingPrompt> _jointLandingPending;
	/// PRD-J10: broker this landing to the commanding seat instead of asking here.
	/// True = brokered (the caller must NOT pop its own dialog).
	bool brokerJointLanding(Craft* craft, Texture* missionTexture, Texture* globeTexture, int shade);

	/// Update list of active crafts.
	const std::vector<Craft*>* updateActiveCrafts();

	void cbxRegionChange(Action *action);
	void cbxZoneChange(Action *action);
	void cbxAreaChange(Action *action);
	void updateZoneInfo();
	void cbxCountryChange(Action *action);

public:
	/// Creates the Geoscape state.
	GeoscapeState();
	/// Cleans up the Geoscape state.
	~GeoscapeState();
	// coop
	void startCoopMission();
	/// Handle keypresses.
	void handle(Action *action) override;
	/// Updates the palette and timer.
	void init() override;
	/// Runs the timer.
	void think() override;
	/// Displays the game time/date. (+Funds)
	void timeDisplay();
	/// Advances the game timer.
	void timeAdvance();
	/// Trigger whenever 5 seconds pass.
	void time5Seconds();
	/// Trigger whenever 10 minutes pass.
	void time10Minutes();
	void ufoHuntingAndEscorting();
	void baseHunting();
	/// Trigger whenever 30 minutes pass.
	void time30Minutes();
	void ufoDetection(Ufo* ufo, const std::vector<Craft*>* activeCrafts);
	/// Trigger whenever 1 hour passes.
	void time1Hour();
	/// Trigger whenever 1 day passes.
	void time1Day();
	/// Trigger whenever 1 month passes.
	void time1Month();
	/// Trigger whenever 1 month passes.
	void time1MonthCoop();
	/// Resets the timer to minimum speed.
	void timerReset();
	/// Displays a popup window.
	void popup(State *state);
	/// Gets the Geoscape globe.
	Globe *getGlobe() const;
	/// Handler for clicking the globe.
	void globeClick(Action *action);
	/// Handler for clicking the Intercept button.
	void btnInterceptClick(Action *action);
	/// Handler for clicking the UFO Tracker button.
	void btnUfoTrackerClick(Action *action);
	/// Handler for clicking the TechTreeViewer button.
	void btnTechTreeViewerClick(Action *action);
	/// Handler for clicking the [SelectMusicTrack] button.
	void btnSelectMusicTrackClick(Action *action);
	/// Handler for clicking the [GlobalProduction] key.
	void btnGlobalProductionClick(Action *action);
	/// Handler for clicking the [GlobalResearch] key.
	void btnGlobalResearchClick(Action *action);
	/// Handler for clicking the [GlobalAlienContainment] key.
	void btnGlobalAlienContainmentClick(Action *action);
	/// Handler for clicking the [DogfightExperience] key.
	void btnDogfightExperienceClick(Action *action);
	/// Handler for clicking the [Debug] key.
	void btnDebugClick(Action *action);
	/// Handler for clicking the Bases button.
	void btnBasesClick(Action *action);
	/// Handler for clicking the Graph button.
	void btnGraphsClick(Action *action);
	/// Handler for clicking the Ufopaedia button.
	void btnUfopaediaClick(Action *action);
	/// Handler for clicking the Options button.
	void btnOptionsClick(Action *action);
	/// Handler for clicking the Funding button.
	void btnFundingClick(Action *action);
	/// Handler for pressing the Rotate Left arrow.
	void btnRotateLeftPress(Action *action);
	/// Handler for releasing the Rotate Left arrow.
	void btnRotateLeftRelease(Action *action);
	/// Handler for pressing the Rotate Right arrow.
	void btnRotateRightPress(Action *action);
	/// Handler for releasing the Rotate Right arrow.
	void btnRotateRightRelease(Action *action);
	/// Handler for pressing the Rotate Up arrow.
	void btnRotateUpPress(Action *action);
	/// Handler for releasing the Rotate Up arrow.
	void btnRotateUpRelease(Action *action);
	/// Handler for pressing the Rotate Down arrow.
	void btnRotateDownPress(Action *action);
	/// Handler for releasing the Rotate Down arrow.
	void btnRotateDownRelease(Action *action);
	/// Handler for left-clicking the Zoom In icon.
	void btnZoomInLeftClick(Action *action);
	/// Handler for right-clicking the Zoom In icon.
	void btnZoomInRightClick(Action *action);
	/// Handler for left-clicking the Zoom Out icon.
	void btnZoomOutLeftClick(Action *action);
	/// Handler for right-clicking the Zoom Out icon.
	void btnZoomOutRightClick(Action *action);
	/// Blit method - renders the state and dogfights.
	void blit() override;
	/// Globe zoom in effect for dogfights.
	void zoomInEffect();
	/// Globe zoom out effect for dogfights.
	void zoomOutEffect();
	/// Multi-dogfights logic handling.
	void handleDogfights();
	void handleDogfightMultiAction(int button);
	/// Dogfight experience handling.
	void handleDogfightExperience();
	/// Gets the number of minimized dogfights.
	int minimizedDogfightsCount();
	/// Starts a new dogfight.
	void startDogfight();
	/// PRD-J08 JOINT: open the dogfight UI for a craft/UFO pair on THIS machine
	/// (the initiating player's replica) - mirrors the vanilla start block.
	void startJointDogfight(Craft* craft, Ufo* ufo);
	/// PRD-J10 JOINT (HOST): the commanding seat answered a brokered landing
	/// prompt. @a yes -> generate the battle exactly as the host's own
	/// ConfirmLandingState would; otherwise patrol here / return to base.
	void jointLandingReply(Craft* craft, bool yes, bool patrol);
	/// Test-harness: is a brokered landing decision outstanding on this host?
	bool hasJointLandingPending() const { return !_jointLandingPending.empty(); }
	/// Test-harness introspection of the live dogfight list.
	const std::list<DogfightState*>& getDogfights() const { return _dogfights; }
	/// Test-harness: dogfights queued but not yet started.
	size_t pendingDogfightCount() const { return _dogfightsToBeStarted.size(); }
	/// Get first free dogfight slot.
	int getFirstFreeDogfightSlot();
	/// Handler for clicking the timer button.
	void btnTimerClick(Action *action);
	/// Selects a time-speed button by index (0=5s,1=1min,2=5min,3=30min,4=1hr,5=1day). For the test harness.
	void setTimeSpeedIndex(int idx);
	/// Updates the co-op ally markers on the speed/toolbar buttons.
	void updatePeerSpeedIndicators();
	/// Tells the other player which geoscape sub-screen this player navigated to (0..5).
	void sendCoopFocus(int screen);
	/// Maps an event popup to the ally-marker toolbar button (0..5), or -1 to leave it yellow (busy).
	int coopFocusForPopup(State *state);
	/// Process a mission site
	bool processMissionSite(MissionSite *site);
	/// Handles base defense
	void handleBaseDefense(Base *base, Ufo *ufo);
	/// Update the resolution settings, we just resized the window.
	void resize(int &dX, int &dY) override;
	/// Handle alien mission generation.
	void determineAlienMissions(bool isNewMonth = true, const RuleEvent* eventRules = nullptr);
private:
	bool attemptAlienRaceEvolution(int month, AlienBase* ab) const;
	/// Process each individual mission script command.
	bool processCommand(RuleMissionScript *command);
	bool buttonsDisabled();
	void updateSlackingIndicator();
};

}
