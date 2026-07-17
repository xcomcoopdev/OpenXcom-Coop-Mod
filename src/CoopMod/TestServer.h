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
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <json/json.h>

namespace OpenXcom
{

class Game;

/**
 * Automation/test command server. Only active when the OXC_TEST_PORT
 * environment variable is set: listens on 127.0.0.1:<port> for one client
 * speaking newline-delimited JSON commands, and executes them on the main
 * thread via pump() (called once per frame from Game::run). Lets an external
 * driver load saves, host/join coop sessions, inspect soldiers/coop state and
 * trigger soldier transfers without a human at the keyboard.
 */
class TestServer
{
public:
	static TestServer& instance();

	// Reads OXC_TEST_PORT; if set, starts the listener thread. Safe to call
	// more than once. Called from Game::run() startup.
	void startFromEnvironment(Game* game);
	// Executes queued commands on the main thread. Called once per frame.
	void pump();
	// Stops the listener thread (called from shutdown).
	void stop();

private:
	TestServer() = default;
	void ioThread(int port);
	std::string execute(const std::string& line);
	/// PRD-J10 hooks. execute()'s command chain is at MSVC's 128-block nesting
	/// limit (C1061), so newer commands get their own dispatcher; execute() tries
	/// this one first. True = @a cmd was handled here.
	bool executeJoint10(const std::string& cmd, const Json::Value& req, Json::Value& resp);

	Game* _game = nullptr;
	std::thread _thread;
	std::atomic<bool> _running{false};
	std::mutex _mutex;
	std::deque<std::string> _inbox;
	std::deque<std::string> _outbox;
};

}
