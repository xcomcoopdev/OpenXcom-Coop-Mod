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
#include "ChatMenu.h"
#include "../Engine/Font.h"
#include "../Engine/Game.h"
#include "../Engine/Surface.h"
#include "../Engine/Unicode.h"
#include <ctime>
#include <sstream>
#include "../Savegame/SavedGame.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/Map.h"
#include "../Battlescape/Camera.h"

#include "../Engine/Unicode.h"

namespace OpenXcom
{

ChatMenu::ChatMenu(Font* gameFont, Game* game) : active(false), _font(gameFont), _game(game) {}

void ChatMenu::setActive(bool state)
{	
	_game->getCoopMod()->_isChatActiveStatic = state;

	active.store(state);
}

bool ChatMenu::isActive() const
{
	return active.load();
}

std::string ChatMenu::getCurrentTime()
{
	std::time_t t = std::time(nullptr);
	std::tm* tm = std::localtime(&t);
	char buffer[6];
	std::strftime(buffer, sizeof(buffer), "%H:%M", tm);
	return std::string(buffer);
}

void ChatMenu::handleEvent(const SDL_Event& event)
{
	if (!isActive())
		return;

	if (event.type == SDL_KEYDOWN)
	{

		if (event.key.keysym.sym == SDLK_BACKSPACE && !inputText.empty())
		{
			// Remove last UTF-8 character properly
			inputText = Unicode::codePointSubstrUTF8(inputText, 0, Unicode::codePointLengthUTF8(inputText) - 1);
		}
		else if (event.key.keysym.sym == SDLK_RETURN)
		{
			if (!inputText.empty())
			{
				ChatMessage msg;
				msg.time = getCurrentTime();
				msg.player = _game->getCoopMod()->getHostName();
				msg.text = inputText;

				if (_game->getCoopMod()->getCoopStatic())
				{
					Json::Value root;
					root["state"] = "chat_message";
					root["msg_time"] = msg.time;
					root["msg_player"] = msg.player;
					root["msg_text"] = msg.text;

					_game->getCoopMod()->sendTCPPacketData(root.toStyledString());
				}

				messages.push_back(msg);
				if (messages.size() > 8)
					messages.erase(messages.begin());

				inputText.clear();
			}
		}
		else if (event.key.keysym.unicode >= 32)
		{
			// Calculate the current length in characters (UTF-8 code points)
			int length = Unicode::codePointLengthUTF8(inputText);

			if (length < 30) // allow only if under 30 characters
			{
				// Convert a single character to UTF-8 using OpenXcom Unicode
				OpenXcom::UString u32char;
				u32char.push_back(static_cast<OpenXcom::UCode>(event.key.keysym.unicode));
				inputText += OpenXcom::Unicode::convUtf32ToUtf8(u32char);
			}
		}
	}
}

void ChatMenu::drawText(SDL_Surface* screen, const std::string& text, int x, int y, int maxWidth)
{
	OpenXcom::UString utext = OpenXcom::Unicode::convUtf8ToUtf32(text);

	int posX = x;
	for (size_t i = 0; i < utext.size(); ++i)
	{
		OpenXcom::UCode uc = utext[i];

		if (uc == ' ')
		{
			posX += _font->getWidth() / 2; // smaller space for actual space
			continue;
		}

		// Wrap if exceeds max width
		if (posX + _font->getWidth() > x + maxWidth)
		{
			y += _font->getHeight() + 2;
			posX = x;
		}

		SurfaceCrop glyph = _font->getChar(uc);
		SDL_Rect dst = {posX, y, 0, 0};
		SDL_Rect crop = *glyph.getCrop();
		Surface* surf = const_cast<Surface*>(glyph.getSurface());
		SDL_BlitSurface(surf->getSurface(), &crop, screen, &dst);

		// Move by actual glyph width, not fixed font width
		posX += crop.w;
	}
}

void ChatMenu::drawMiniChat(SDL_Surface* screen)
{
	if (isActive() || !_font || miniMessages.empty())
		return;

	int x = 10;
	int y = 10;

	for (const auto& msg : miniMessages)
	{
		std::string displayText = msg.player + ": " + msg.text;
		drawText(screen, displayText, x, y, 250);
		y += _font->getHeight() + 2;
	}
}

void ChatMenu::updateMiniChat()
{
	Uint32 now = SDL_GetTicks();
	if (!miniMessages.empty() && (now - lastMiniChatClear) >= 5000) // 5 seconds
	{
		miniMessages.erase(miniMessages.begin()); // delete oldest
		lastMiniChatClear = now;
	}
}

void ChatMenu::update()
{

}

void ChatMenu::draw(SDL_Surface* screen)
{
	if (!isActive() || !_font)
		return;

	// Chat window near top-left, slightly bigger for readability
	SDL_Rect chatBox = {10, 10, 220, 140}; // Increased height for coordinates

	// Semi-transparent background
	SDL_Surface* overlay = SDL_CreateRGBSurface(SDL_SRCALPHA, chatBox.w, chatBox.h, 32,
												screen->format->Rmask, screen->format->Gmask, screen->format->Bmask, screen->format->Amask);
	SDL_FillRect(overlay, nullptr, SDL_MapRGBA(screen->format, 30, 30, 30, 255));
	SDL_SetAlpha(overlay, SDL_SRCALPHA, 230); // ~10% transparent
	SDL_BlitSurface(overlay, nullptr, screen, &chatBox);
	SDL_FreeSurface(overlay);

	// Messages
	int y = chatBox.y + 5;
	for (const auto& msg : messages)
	{
		std::string fullMsg = "[" + msg.time + "] " + msg.player + ": " + msg.text;
		drawText(screen, fullMsg, chatBox.x + 5, y, chatBox.w - 10);
		y += _font->getHeight() + 2;
	}

	// Input box
	SDL_Rect inputBox = {chatBox.x + 5, chatBox.y + chatBox.h - (_font->getHeight() + 6), chatBox.w - 10, _font->getHeight() + 4};
	SDL_FillRect(screen, &inputBox, SDL_MapRGB(screen->format, 40, 40, 40));

	if (inputText.empty())
		drawText(screen, "Type a message...", inputBox.x + 3, inputBox.y + 2, inputBox.w - 6);
	else
		drawText(screen, inputText, inputBox.x + 3, inputBox.y + 2, inputBox.w - 6);

	// === Camera coordinates with safe null checks ===
	bool hasCamera = false;
	Position camera_position = {0, 0, 0};

	// Draw ping/latency if available
	std::string ping = _game->getCoopMod()->getPing();
	if (_game->getCoopMod()->isCoopSession() == true && _game->getCoopMod()->getCoopStatic() == true)
	{
		std::string latencyText = "Latency: " + ping + " ms";
		drawText(screen, latencyText, chatBox.x + 5, inputBox.y - (_font->getHeight() * 2 + 8), chatBox.w - 10);
	}

	if (_game)
	{
		auto savedGame = _game->getSavedGame();
		if (savedGame)
		{
			auto savedBattle = savedGame->getSavedBattle();
			if (savedBattle)
			{
				auto battleGame = savedBattle->getBattleGame();
				if (battleGame)
				{
					auto map = battleGame->getMap();
					if (map)
					{
						auto camera = map->getCamera();
						if (camera)
						{
							camera_position = camera->getCenterPosition();
							hasCamera = true;
						}
					}
				}
			}
		}
	}

	// Draw camera coordinates ONLY if available
	if (hasCamera)
	{
		std::string cameraText = "Camera X:" + std::to_string(camera_position.x) +
								 " Y:" + std::to_string(camera_position.y) +
								 " Z:" + std::to_string(camera_position.z);

		drawText(screen, cameraText, chatBox.x + 5, inputBox.y - (_font->getHeight() + 4), chatBox.w - 10);
	}
}

void ChatMenu::addMessage(std::string time, std::string player, std::string text)
{
	ChatMessage msg;
	msg.time = time;
	msg.player = player;
	msg.text = text;

	// Add to main chat
	messages.push_back(msg);
	if (messages.size() > 8)
		messages.erase(messages.begin());

	// Add to mini chat
	miniMessages.push_back(msg);
	if (miniMessages.size() > 8)
		miniMessages.erase(miniMessages.begin());

	// Reset mini chat timer so new message is guaranteed to show for 10s
	lastMiniChatClear = SDL_GetTicks();
}

void ChatMenu::clearMessages()
{
	messages.clear();
	miniMessages.clear();
}

}

