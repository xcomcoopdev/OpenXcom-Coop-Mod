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
#include "Texture.h"
#include "../Savegame/Target.h"
#include "../Engine/RNG.h"

namespace OpenXcom
{

/**
 * Initializes a globe texture.
 * @param id Texture identifier.
 */
Texture::Texture(int id) : _id(id), _fakeUnderwater(false)
{
}

/**
 *
 */
Texture::~Texture()
{
}

/**
 * Loads the texture type from a YAML file.
 * @param node YAML node.
 */
void Texture::load(const YAML::Node &node)
{
	_id = node["id"].as<int>(_id);
	_fakeUnderwater = node["fakeUnderwater"].as<bool>(_fakeUnderwater);
	_startingCondition = node["startingCondition"].as<std::string>(_startingCondition);
	_deployments = node["deployments"].as< std::map<std::string, int> >(_deployments);
	_terrain = node["terrain"].as< std::vector<TerrainCriteria> >(_terrain);
	_baseTerrain = node["baseTerrain"].as< std::vector<TerrainCriteria> >(_baseTerrain);
}

/**
 * Returns the list of terrain criteria associated
 * with this texture.
 * @return List of terrain.
 */
std::vector<TerrainCriteria> *Texture::getTerrain()
{
	return &_terrain;
}

/**
 * Calculates a random terrain for a mission target based
 * on the texture's available terrain criteria.
 * @param target Pointer to the mission target.
 * @return the name of the picked terrain.
 */
std::string Texture::getRandomTerrain(Target *target) const
{
	int totalWeight = 0;
	std::map<int, std::string> possibilities;
	for (const auto& terrainCrit : _terrain)
	{
		if (terrainCrit.weight > 0 &&
			target->getLongitude() >= terrainCrit.lonMin && target->getLongitude() < terrainCrit.lonMax &&
			target->getLatitude() >= terrainCrit.latMin && target->getLatitude() < terrainCrit.latMax)
		{
			totalWeight += terrainCrit.weight;
			possibilities[totalWeight] = terrainCrit.name;
		}
	}
	if (totalWeight > 0)
	{
		int pick = RNG::generate(1, totalWeight);
		for (const auto& pair : possibilities)
		{
			if (pick <= pair.first)
			{
				return pair.second;
			}
		}
	}
	return "";
}

/**
 * Returns the list of terrain criteria associated
 * with this texture for base defense missions.
 * @return List of terrain.
 */
std::vector<TerrainCriteria> *Texture::getBaseTerrain()
{
	return &_baseTerrain;
}

/**
 * Calculates a random terrain for a base defense mission target based
 * on the texture's available terrain criteria.
 * @param target Pointer to the mission target.
 * @return the name of the picked terrain.
 */
std::string Texture::getRandomBaseTerrain(Target *target) const
{
	int totalWeight = 0;
	std::map<int, std::string> possibilities;
	for (const auto& terrainCrit : _baseTerrain)
	{
		if (terrainCrit.weight > 0 &&
			target->getLongitude() >= terrainCrit.lonMin && target->getLongitude() < terrainCrit.lonMax &&
			target->getLatitude() >= terrainCrit.latMin && target->getLatitude() < terrainCrit.latMax)
		{
			totalWeight += terrainCrit.weight;
			possibilities[totalWeight] = terrainCrit.name;
		}
	}
	int pick = RNG::generate(1, totalWeight);
	for (const auto& pair : possibilities)
	{
		if (pick <= pair.first)
		{
			return pair.second;
		}
	}
	return "";
}

/**
 * Returns the list of deployments associated
 * with this texture.
 * @return List of deployments.
 */
const std::map<std::string, int> &Texture::getDeployments() const
{
	return _deployments;
}

/**
 * Calculates a random deployment for a mission target based
 * on the texture's available deployments.
 * @return the name of the picked deployment.
 */
std::string Texture::getRandomDeployment() const
{
	if (_deployments.empty())
	{
		return "";
	}
	if (_deployments.size() == 1)
	{
		return _deployments.begin()->first;
	}

	int totalWeight = 0;
	for (const auto& pair : _deployments)
	{
		totalWeight += pair.second;
	}

	if (totalWeight >= 1)
	{
		int pick = RNG::generate(1, totalWeight);
		for (const auto& pair : _deployments)
		{
			if (pick <= pair.second)
			{
				return pair.first;
			}
			else
			{
				pick -= pair.second;
			}
		}
	}

	return "";
}

}
