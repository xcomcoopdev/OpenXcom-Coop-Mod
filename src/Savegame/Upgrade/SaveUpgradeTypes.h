#pragma once
/*
 * Copyright 2010-2026 OpenXcom Developers.
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

// In-game Save Upgrader - INTERNAL transform types.
//
// This header exposes rapidyaml, so only the upgrade module's .cpp files include
// it. It defines the raw-YAML SaveSet model (PRD 6.1), the SchemaStep interface
// and registry (PRD 6.2), and small ryml mutation helpers shared by ingest/emit
// and the individual steps.

#include <string>
#include <vector>
#include <memory>

#include "../../Engine/Yaml.h" // rapidyaml (ryml)
#include "SaveUpgrade.h"

namespace OpenXcom
{

namespace SaveUpgrade
{

////////////////////////////////////////////////////////////
//  ryml mutation / query helpers (defined in SaveUpgrade.cpp)
////////////////////////////////////////////////////////////

namespace yamlutil
{

/// Find (or create) a keyed child under a map node. Ensures map is a mapping.
ryml::NodeRef mapChild(ryml::NodeRef map, ryml::csubstr key);

/// Set (overwrite) a string value under key.
void setStr(ryml::NodeRef map, ryml::csubstr key, const std::string& val);
/// Set (overwrite) a boolean value under key, emitted as true/false to match the engine.
void setBool(ryml::NodeRef map, ryml::csubstr key, bool value);
/// Set (overwrite) an integer value under key.
void setInt(ryml::NodeRef map, ryml::csubstr key, long long value);

/// True if a readable map node has the given key.
bool hasKey(ryml::ConstNodeRef map, ryml::csubstr key);
/// Read a string value, or def if missing/null.
std::string getStr(ryml::ConstNodeRef map, ryml::csubstr key, const std::string& def = std::string());
/// Read an integer value, or def if missing/unparsable.
long long getInt(ryml::ConstNodeRef map, ryml::csubstr key, long long def = 0);
/// Read a boolean value, or def if missing.
bool getBool(ryml::ConstNodeRef map, ryml::csubstr key, bool def = false);

/// Remove a key from a map if present (no-op otherwise).
void removeKey(ryml::NodeRef map, ryml::csubstr key);

/// Split a two-document save stream ("header\n---\nbody") into its two halves.
/// Returns false if no document separator line is found.
bool splitStream(const std::string& text, std::string& header, std::string& body);

/// Parse a single YAML mapping document into a fresh owning tree.
std::shared_ptr<ryml::Tree> parseMap(const std::string& text);

/// Emit a node (map) to a YAML string (no document markers).
std::string emitNode(ryml::ConstNodeRef node);

/// base64-decode a scalar into raw bytes.
std::string base64DecodeToString(ryml::csubstr encoded);

} // namespace yamlutil

////////////////////////////////////////////////////////////
//  SaveSet model (PRD 6.1)
////////////////////////////////////////////////////////////

/// One decoded two-document world: header map + body map, each in its own
/// mutable ryml tree so steps can transform fields in place and re-emit.
struct SaveWorld
{
	std::shared_ptr<ryml::Tree> headerTree;
	std::shared_ptr<ryml::Tree> bodyTree;

	ryml::NodeRef header() { return headerTree->rootref(); }
	ryml::NodeRef body() { return bodyTree->rootref(); }
	ryml::ConstNodeRef header() const { return headerTree->crootref(); }
	ryml::ConstNodeRef body() const { return bodyTree->crootref(); }

	bool valid() const { return headerTree && bodyTree; }

	/// Parse a two-document stream string into this world (throws on failure).
	void ingestStream(const std::string& stream);
	/// Serialize back to a single two-document stream string ("header\n---\nbody").
	std::string emit() const;
};

/// A logical client world plus the name it is keyed under.
struct ClientWorld
{
	std::string name;
	SaveWorld world;
};

/// The whole set of worlds a save decodes into, plus migration metadata.
struct SaveSet
{
	int schema = 0;
	SchemaVariant variant = SchemaVariant::None;

	SaveWorld host;
	std::vector<ClientWorld> clients;

	long long saveID = 0;
	int gamemode = 0;
	std::vector<std::string> roster; // exactly [hostOrBlank, clientName] after a 1->2 step

	std::vector<std::string> report; // human-readable lines accumulated by steps
};

////////////////////////////////////////////////////////////
//  Step registry (PRD 6.2)
////////////////////////////////////////////////////////////

/// One schema bump. Old steps are frozen; a new format change adds one file.
class SchemaStep
{
public:
	virtual ~SchemaStep() = default;

	virtual int fromSchema() const = 0;
	virtual int toSchema() const = 0;

	/// Inputs the UI must collect before this step can run (empty = silent step).
	virtual std::vector<InputRequest> requiredInputs(const SaveSet& set) const = 0;

	/// Programmatic pre-flight checks. Appends blocking errors / warnings to out.
	virtual void validate(const SaveSet& set, const UpgradeInputs& in, PreflightResult& out) const = 0;

	/// Pure transform: mutate the SaveSet's host/client docs and append report lines.
	virtual void apply(SaveSet& set, const UpgradeInputs& in) const = 0;
};

/// The concrete step accessor (defined in SchemaStep1to2.cpp).
const SchemaStep* step_1_to_2();

/// All registered steps, ordered so a chain can be built by (from == schema).
const std::vector<const SchemaStep*>& getSchemaSteps();

/// The blob-store key a client world is embedded under (matches connectionTCP::hostBlobKey).
std::string hostBlobKey(long long saveID, const std::string& clientName);

} // namespace SaveUpgrade

} // namespace OpenXcom
