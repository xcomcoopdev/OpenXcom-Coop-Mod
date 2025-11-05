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
#include <SDL.h>
#include <atomic>
#include <string>
#include <vector>

namespace OpenXcom
{
class Font;
class Game;

struct ChatMessage
{
	std::string time;
	std::string player;
	std::string text;
};

class ChatMenu
{
  public:
	ChatMenu(OpenXcom::Font* gameFont, OpenXcom::Game* game);
	void setActive(bool state);
	bool isActive() const;
	void handleEvent(const SDL_Event& event);
	void draw(SDL_Surface* screen);
	void addMessage(std::string time, std::string player, std::string text);
	void clearMessages();
	void drawMiniChat(SDL_Surface* screen);
	void updateMiniChat();
	void update();

  private:
	std::atomic<bool> active;
	std::vector<ChatMessage> messages;
	std::vector<ChatMessage> miniMessages;
	Uint32 lastChatClear = 0;
	Uint32 lastMiniChatClear = 0;
	Uint32 pingSentTime = 0;
	std::string inputText;
	OpenXcom::Font* _font;
	OpenXcom::Game* _game;
	void drawText(SDL_Surface* screen, const std::string& text, int x, int y, int maxWidth);
	std::string getCurrentTime();
};

}

