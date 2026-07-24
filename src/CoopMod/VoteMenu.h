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
 */

#include "../Engine/State.h"

#include <cstdint>
#include <string>
#include <vector>

namespace OpenXcom
{

// Use one project-specific name for the player-name list in both the
// declaration and definition. JsonCpp defines Json::Value::Members as the
// same underlying std::vector<std::string> type, which can confuse Visual
// Studio IntelliSense when it displays the canonical alias.
using VotePlayerNames = std::vector<std::string>;

class Action;
class Text;
class TextButton;
class Window;

/**
 * Multiplayer yes/no vote popup.
 *
 * The host owns the vote result. This menu only submits the local seat's vote
 * and renders the host-provided seat-by-seat snapshot.
 */
class VoteMenu : public State
{
private:
	Window *_window;
	Text *_txtTitle;
	Text *_txtQuestion;
	Text *_txtRule;
	Text *_txtPlayers;
	Text *_txtStatus;
	TextButton *_btnYes;
	TextButton *_btnNo;
	TextButton *_btnClose;

	std::uint64_t _voteId;
	int _localSeat;
	int _totalPlayers;
	int _requiredYesVotes;
	std::vector<int> _votes;
	// Names are copied from the host's vote_start packet in the same order as
	// the vote array, so every machine shows the same identities.
	VotePlayerNames _playerNames;
	bool _submitted;
	bool _finished;

	void submitVote(bool yes);
	void refreshPlayerRows();
	void refreshStatus();
	std::string playerName(int seat) const;

public:
	VoteMenu(
		std::uint64_t voteId,
		const std::string &title,
		const std::string &question,
		int totalPlayers,
		int requiredYesVotes,
		const VotePlayerNames &playerNames);
	~VoteMenu() override;

	void btnYesClick(Action *action);
	void btnNoClick(Action *action);
	void btnCloseClick(Action *action);

	std::uint64_t getVoteId() const { return _voteId; }
	bool isFinished() const { return _finished; }
	const VotePlayerNames& getPlayerNames() const { return _playerNames; }
	std::string getPlayerRowsText() const;

	void setVotes(const std::vector<int> &votes);
	void finishVote(bool passed);
};

}
