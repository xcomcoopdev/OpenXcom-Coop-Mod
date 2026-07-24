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

#include "VoteMenu.h"

#include <algorithm>
#include <sstream>

#include "connectionTCP.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Savegame/SavedGame.h"

namespace OpenXcom
{

VoteMenu::VoteMenu(
	std::uint64_t voteId,
	const std::string &title,
	const std::string &question,
	int totalPlayers,
	int requiredYesVotes,
	const VotePlayerNames &playerNames)
	: _voteId(voteId),
	  _localSeat(connectionTCP::localSeat()),
	  _totalPlayers(std::max(1, totalPlayers)),
	  _requiredYesVotes(std::max(1, requiredYesVotes)),
	  _votes(static_cast<std::size_t>(std::max(1, totalPlayers)), VoteSession::NOT_VOTED),
	  _playerNames(static_cast<std::size_t>(std::max(1, totalPlayers))),
	  _submitted(false),
	  _finished(false)
{
	// The host sends a name snapshot together with vote_start. Copy it once so
	// later save swaps or lobby teardown cannot turn the rows back into generic
	// PLAYER 1 / PLAYER 2 labels while the vote popup is still open.
	const std::size_t nameCount = std::min(_playerNames.size(), playerNames.size());
	for (std::size_t i = 0; i < nameCount; ++i)
	{
		_playerNames[i] = playerNames[i];
	}

	_screen = false;

	const int x = 10;
	const int y = 5;

	_window = new Window(this, 300, 190, x, y, POPUP_BOTH);
	_txtTitle = new Text(280, 17, x + 10, y + 10);
	_txtQuestion = new Text(264, 34, x + 18, y + 30);
	_txtRule = new Text(264, 17, x + 18, y + 67);
	_txtPlayers = new Text(264, 58, x + 18, y + 84);
	_txtStatus = new Text(264, 17, x + 18, y + 144);
	_btnYes = new TextButton(126, 18, x + 18, y + 166);
	_btnNo = new TextButton(126, 18, x + 156, y + 166);
	_btnClose = new TextButton(264, 18, x + 18, y + 166);

	setInterface(
		"pauseMenu",
		false,
		_game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : nullptr);

	add(_window, "window", "pauseMenu");
	add(_txtTitle, "text", "pauseMenu");
	add(_txtQuestion, "text", "pauseMenu");
	add(_txtRule, "text", "pauseMenu");
	add(_txtPlayers, "text", "pauseMenu");
	add(_txtStatus, "text", "pauseMenu");
	add(_btnYes, "button", "pauseMenu");
	add(_btnNo, "button", "pauseMenu");
	add(_btnClose, "button", "pauseMenu");

	centerAllSurfaces();
	setWindowBackground(_window, "pauseMenu");

	if (_game->getSavedGame() && _game->getSavedGame()->getSavedBattle())
	{
		applyBattlescapeTheme("pauseMenu");
	}

	_txtTitle->setBig();
	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setText(title.empty() ? "VOTE" : title);

	_txtQuestion->setBig();
	_txtQuestion->setAlign(ALIGN_CENTER);
	_txtQuestion->setVerticalAlign(ALIGN_MIDDLE);
	_txtQuestion->setWordWrap(true);
	_txtQuestion->setText(question);

	_txtRule->setSmall();
	_txtRule->setAlign(ALIGN_CENTER);
	_txtRule->setText(
		"STRICT MAJORITY: " + std::to_string(_requiredYesVotes) +
		" OF " + std::to_string(_totalPlayers) + " YES VOTES");

	_txtPlayers->setSmall();
	_txtPlayers->setAlign(ALIGN_CENTER);

	_txtStatus->setSmall();
	_txtStatus->setAlign(ALIGN_CENTER);

	_btnYes->setText(tr("STR_YES"));
	_btnYes->onMouseClick((ActionHandler)&VoteMenu::btnYesClick);
	_btnYes->onKeyboardPress((ActionHandler)&VoteMenu::btnYesClick, Options::keyOk);

	_btnNo->setText(tr("STR_NO"));
	_btnNo->onMouseClick((ActionHandler)&VoteMenu::btnNoClick);
	_btnNo->onKeyboardPress((ActionHandler)&VoteMenu::btnNoClick, Options::keyCancel);

	_btnClose->setText("CLOSE");
	_btnClose->onMouseClick((ActionHandler)&VoteMenu::btnCloseClick);
	_btnClose->onKeyboardPress((ActionHandler)&VoteMenu::btnCloseClick, Options::keyOk);
	_btnClose->onKeyboardPress((ActionHandler)&VoteMenu::btnCloseClick, Options::keyCancel);
	_btnClose->setVisible(false);

	refreshPlayerRows();
	refreshStatus();
}

VoteMenu::~VoteMenu()
{
}

std::string VoteMenu::getPlayerRowsText() const
{
	return _txtPlayers ? _txtPlayers->getText() : std::string();
}

std::string VoteMenu::playerName(int seat) const
{
	// Prefer the authoritative snapshot carried by this vote. The local saved
	// roster is only a fallback for old packets or a partially initialized save.
	std::string name;
	if (seat >= 0 && static_cast<std::size_t>(seat) < _playerNames.size())
	{
		name = _playerNames[static_cast<std::size_t>(seat)];
	}
	if (name.empty())
	{
		name = connectionTCP::seatName(seat);
	}
	if (name.empty())
	{
		name = "PLAYER " + std::to_string(seat + 1);
	}
	if (name.size() > 22)
	{
		name.resize(22);
	}
	return name;
}

void VoteMenu::refreshPlayerRows()
{
	std::ostringstream rows;
	for (int seat = 0; seat < _totalPlayers; ++seat)
	{
		if (seat != 0)
		{
			rows << '\n';
		}

		const int value = seat < static_cast<int>(_votes.size())
			? _votes[static_cast<std::size_t>(seat)]
			: VoteSession::NOT_VOTED;

		rows << playerName(seat);
		if (seat == _localSeat)
		{
			rows << " (YOU)";
		}
		rows << ": ";

		if (value == VoteSession::VOTED_YES)
		{
			rows << "YES";
		}
		else if (value == VoteSession::VOTED_NO)
		{
			rows << "NO";
		}
		else
		{
			rows << "WAITING";
		}
	}
	_txtPlayers->setText(rows.str());
}

void VoteMenu::refreshStatus()
{
	if (_finished)
	{
		return;
	}

	int yesVotes = 0;
	int noVotes = 0;
	for (int vote : _votes)
	{
		if (vote == VoteSession::VOTED_YES)
		{
			++yesVotes;
		}
		else if (vote == VoteSession::VOTED_NO)
		{
			++noVotes;
		}
	}

	if (_submitted)
	{
		_txtStatus->setText(
			"VOTE SENT - YES: " + std::to_string(yesVotes) +
			"  NO: " + std::to_string(noVotes));
	}
	else
	{
		_txtStatus->setText(
			"CAST YOUR VOTE - YES: " + std::to_string(yesVotes) +
			"  NO: " + std::to_string(noVotes));
	}
}

void VoteMenu::submitVote(bool yes)
{
	if (_submitted || _finished)
	{
		return;
	}

	// Hide the controls before calling into the vote controller. A host-side
	// vote can resolve synchronously and close this state through the voted
	// action, so this method must not touch members after castVote().
	_submitted = true;
	_btnYes->setVisible(false);
	_btnNo->setVisible(false);
	_txtStatus->setText(yes ? "SENDING YES VOTE..." : "SENDING NO VOTE...");

	_game->getCoopMod()->castVote(_voteId, yes);
}

void VoteMenu::btnYesClick(Action *)
{
	submitVote(true);
}

void VoteMenu::btnNoClick(Action *)
{
	submitVote(false);
}

void VoteMenu::btnCloseClick(Action *)
{
	if (!_finished)
	{
		return;
	}

	_game->popState();
}

void VoteMenu::setVotes(const std::vector<int> &votes)
{
	_votes.assign(static_cast<std::size_t>(_totalPlayers), VoteSession::NOT_VOTED);
	const std::size_t count = std::min(_votes.size(), votes.size());
	for (std::size_t i = 0; i < count; ++i)
	{
		const int value = votes[i];
		if (value == VoteSession::VOTED_YES || value == VoteSession::VOTED_NO)
		{
			_votes[i] = value;
		}
	}

	if (_localSeat >= 0 && _localSeat < static_cast<int>(_votes.size())
		&& _votes[static_cast<std::size_t>(_localSeat)] != VoteSession::NOT_VOTED)
	{
		_submitted = true;
		_btnYes->setVisible(false);
		_btnNo->setVisible(false);
	}

	refreshPlayerRows();
	refreshStatus();
}

void VoteMenu::finishVote(bool passed)
{
	_finished = true;
	_submitted = true;

	_btnYes->setVisible(false);
	_btnNo->setVisible(false);
	_btnClose->setVisible(true);

	_txtStatus->setText(passed ? "VOTE PASSED" : "VOTE FAILED");
}

}
