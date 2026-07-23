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

#include "SaveUpgradeTypes.h"

#include <iterator>
#include <string>
#include <tuple>
#include <c4/base64.hpp>

#include "../../Engine/Options.h"
#include "../../Engine/CrossPlatform.h"
#include "../../Engine/Exception.h"
#include "../../Engine/Logger.h"

namespace OpenXcom
{

namespace SaveUpgrade
{

////////////////////////////////////////////////////////////
//  ryml helpers
////////////////////////////////////////////////////////////

namespace yamlutil
{

ryml::NodeRef mapChild(ryml::NodeRef map, ryml::csubstr key)
{
	if (!map.is_map())
		map |= ryml::MAP;
	ryml::NodeRef c = map.find_child(key);
	if (!c.invalid())
		return c;
	c = map.append_child();
	c.set_key(map.tree()->to_arena(key));
	return c;
}

void setStr(ryml::NodeRef map, ryml::csubstr key, const std::string& val)
{
	ryml::NodeRef c = mapChild(map, key);
	c.set_val(c.tree()->to_arena(ryml::to_csubstr(val)));
}

void setBool(ryml::NodeRef map, ryml::csubstr key, bool value)
{
	setStr(map, key, value ? "true" : "false");
}

void setInt(ryml::NodeRef map, ryml::csubstr key, long long value)
{
	ryml::NodeRef c = mapChild(map, key);
	c.set_val_serialized(value);
}

bool hasKey(ryml::ConstNodeRef map, ryml::csubstr key)
{
	if (map.invalid() || !map.is_map())
		return false;
	return !map.find_child(key).invalid();
}

std::string getStr(ryml::ConstNodeRef map, ryml::csubstr key, const std::string& def)
{
	if (map.invalid() || !map.is_map())
		return def;
	ryml::ConstNodeRef c = map.find_child(key);
	if (c.invalid() || !c.has_val() || c.val_is_null())
		return def;
	ryml::csubstr v = c.val();
	return std::string(v.str, v.len);
}

long long getInt(ryml::ConstNodeRef map, ryml::csubstr key, long long def)
{
	std::string s = getStr(map, key, std::string());
	if (s.empty())
		return def;
	try
	{
		return std::stoll(s);
	}
	catch (...)
	{
		return def;
	}
}

bool getBool(ryml::ConstNodeRef map, ryml::csubstr key, bool def)
{
	std::string s = getStr(map, key, std::string());
	if (s.empty())
		return def;
	return s == "true" || s == "True" || s == "1" || s == "yes";
}

void removeKey(ryml::NodeRef map, ryml::csubstr key)
{
	if (map.invalid() || !map.is_map())
		return;
	if (!map.find_child(key).invalid())
		map.remove_child(key);
}

bool splitStream(const std::string& text, std::string& header, std::string& body)
{
	const size_t n = text.size();
	size_t pos = 0;
	while (pos < n)
	{
		size_t nl = text.find('\n', pos);
		size_t lineEnd = (nl == std::string::npos) ? n : nl;
		size_t e = lineEnd;
		if (e > pos && text[e - 1] == '\r')
			--e;
		// Is this line exactly "---" (tolerating trailing whitespace)?
		bool isSep = false;
		if (e - pos >= 3 && text.compare(pos, 3, "---") == 0)
		{
			isSep = true;
			for (size_t i = pos + 3; i < e; ++i)
			{
				if (text[i] != ' ' && text[i] != '\t')
				{
					isSep = false;
					break;
				}
			}
		}
		if (isSep)
		{
			header.assign(text, 0, pos);
			if (nl == std::string::npos)
				body.clear();
			else
				body.assign(text, nl + 1, n - (nl + 1));
			return true;
		}
		if (nl == std::string::npos)
			break;
		pos = nl + 1;
	}
	return false;
}

std::shared_ptr<ryml::Tree> parseMap(const std::string& text)
{
	auto tree = std::make_shared<ryml::Tree>();
	ryml::parse_in_arena(ryml::to_csubstr(text), tree.get());
	return tree;
}

std::string emitNode(ryml::ConstNodeRef node)
{
	return ryml::emitrs_yaml<std::string>(node);
}

std::string base64DecodeToString(ryml::csubstr encoded)
{
	if (encoded.len == 0)
		return std::string();
	// Allocate a safe upper bound (4 base64 chars -> at most 3 bytes) and let
	// base64_decode report the exact number of bytes it wrote.
	std::string out;
	out.resize((encoded.len / 4 + 1) * 3);
	size_t actual = c4::base64_decode(encoded, ryml::blob(static_cast<void*>(&out[0]), out.size()));
	out.resize(actual <= out.size() ? actual : 0);
	return out;
}

} // namespace yamlutil

////////////////////////////////////////////////////////////
//  SaveWorld
////////////////////////////////////////////////////////////

void SaveWorld::ingestStream(const std::string& stream)
{
	std::string h, b;
	if (!yamlutil::splitStream(stream, h, b))
		throw Exception("save stream is not a two-document YAML stream");
	headerTree = yamlutil::parseMap(h);
	bodyTree = yamlutil::parseMap(b);
}

std::string SaveWorld::emit() const
{
	std::string out = yamlutil::emitNode(header());
	if (out.empty() || out.back() != '\n')
		out.push_back('\n');
	out += "---\n";
	out += yamlutil::emitNode(body());
	return out;
}

std::string hostBlobKey(long long saveID, const std::string& clientName)
{
	return "host_" + std::to_string(saveID) + "_" + clientName + ".data";
}

////////////////////////////////////////////////////////////
//  file-local helpers
////////////////////////////////////////////////////////////

namespace
{

std::string readFileText(const std::string& fileName)
{
	std::string full = Options::getMasterUserFolder() + fileName;
	auto in = CrossPlatform::readFile(full);
	if (!in)
		throw Exception("could not read " + full);
	return std::string((std::istreambuf_iterator<char>(*in)), std::istreambuf_iterator<char>());
}

// host_<saveID>_<name>.data -> <name>  (mirror of connectionTCP::findHostClientBlob)
std::string clientNameFromKey(const std::string& key)
{
	const std::string prefix = "host_";
	const std::string ext = ".data";
	if (key.size() < prefix.size() + ext.size() || key.compare(0, prefix.size(), prefix) != 0)
		return std::string();
	size_t idEnd = key.find('_', prefix.size());
	if (idEnd == std::string::npos)
		return std::string();
	size_t nameStart = idEnd + 1;
	size_t nameEnd = key.size();
	if (key.size() >= ext.size() && key.compare(key.size() - ext.size(), ext.size(), ext) == 0)
		nameEnd = key.size() - ext.size();
	if (nameEnd <= nameStart)
		return std::string();
	return key.substr(nameStart, nameEnd - nameStart);
}

bool sidecarPresent(const std::string& folder, long long saveID)
{
	const std::string prefix = "host_" + std::to_string(saveID) + "_";
	for (const auto& item : CrossPlatform::getFolderContents(folder, "data"))
	{
		const std::string& e = std::get<0>(item);
		if (e.size() > prefix.size() && e.compare(0, prefix.size(), prefix) == 0)
			return true;
	}
	return false;
}

std::string joinLines(const std::vector<std::string>& v, const std::string& sep)
{
	std::string out;
	for (size_t i = 0; i < v.size(); ++i)
	{
		if (i)
			out += sep;
		out += v[i];
	}
	return out;
}

std::vector<const SchemaStep*> buildChain(int from)
{
	std::vector<const SchemaStep*> chain;
	int cur = from;
	// Guard against a malformed registry that could otherwise loop forever.
	for (int guard = 0; cur < SAVE_SCHEMA_CURRENT && guard < 64; ++guard)
	{
		const SchemaStep* next = nullptr;
		for (const SchemaStep* s : getSchemaSteps())
		{
			if (s->fromSchema() == cur)
			{
				next = s;
				break;
			}
		}
		if (!next)
			break; // missing link
		chain.push_back(next);
		cur = next->toSchema();
	}
	return chain;
}

// Framework ingestion (PRD 6.1): decode the host + every client world into the
// SaveSet according to the detected variant. Never routes through SavedGame.
void ingestSaveSet(SaveSet& set, const DetectedSchema& d, const UpgradeInputs& in, const std::string& fileName)
{
	set.schema = d.schema;
	set.variant = d.variant;

	set.host.ingestStream(readFileText(fileName));
	set.saveID = yamlutil::getInt(set.host.body(), "saveID", 0);
	set.gamemode = static_cast<int>(yamlutil::getInt(set.host.body(), "coop_gamemode", 0));

	switch (d.variant)
	{
	case SchemaVariant::Embed:
	{
		ryml::ConstNodeRef body = set.host.body();
		std::string key = yamlutil::getStr(body, "coopClientSaveKey", std::string());
		ryml::ConstNodeRef blobNode = body.invalid() ? body : body.find_child("coopClientSaveBlob");
		if (!blobNode.invalid() && blobNode.has_val())
		{
			std::string clientStream = yamlutil::base64DecodeToString(blobNode.val());
			if (!clientStream.empty())
			{
				ClientWorld cw;
				cw.name = clientNameFromKey(key);
				cw.world.ingestStream(clientStream);
				set.clients.push_back(std::move(cw));
			}
		}
		break;
	}
	case SchemaVariant::Sidecar:
	{
		const std::string prefix = "host_" + std::to_string(set.saveID) + "_";
		const std::string ext = ".data";
		for (const auto& item : CrossPlatform::getFolderContents(Options::getMasterUserFolder(), "data"))
		{
			const std::string& e = std::get<0>(item);
			if (e.size() <= prefix.size() + ext.size() || e.compare(0, prefix.size(), prefix) != 0)
				continue;
			std::string content = readFileText(e);
			if (content.empty())
				continue;
			ClientWorld cw;
			cw.name = e.substr(prefix.size(), e.size() - prefix.size() - ext.size());
			cw.world.ingestStream(content);
			set.clients.push_back(std::move(cw));
		}
		break;
	}
	case SchemaVariant::Dual:
	{
		if (!in.skipClient && !in.clientSaveFileName.empty())
		{
			ClientWorld cw;
			cw.name = in.clientName;
			cw.world.ingestStream(readFileText(in.clientSaveFileName));
			set.clients.push_back(std::move(cw));
		}
		break;
	}
	default:
		break;
	}
}

// Emit (PRD 6.1): re-embed every client world as a base64 coopClientSaves entry,
// then serialize the host stream. Encoding is confined here so future steps
// transform nested worlds automatically.
std::string emitHostStream(SaveSet& set)
{
	ryml::NodeRef hostBody = set.host.body();
	yamlutil::removeKey(hostBody, "coopClientSaves");
	if (!set.clients.empty())
	{
		ryml::NodeRef seq = yamlutil::mapChild(hostBody, "coopClientSaves");
		seq |= ryml::SEQ;
		for (ClientWorld& c : set.clients)
		{
			std::string clientStream = c.world.emit();
			ryml::NodeRef e = seq.append_child();
			e |= ryml::MAP;
			yamlutil::setStr(e, "key", hostBlobKey(set.saveID, c.name));
			ryml::NodeRef blob = yamlutil::mapChild(e, "blob");
			blob.set_val_serialized(c4::fmt::base64(ryml::csubstr(clientStream.data(), clientStream.size())));
		}
	}
	return set.host.emit();
}

// Post-flight (PRD 7): re-parse the emitted stream and assert the upgraded shape.
bool postflightVerify(const std::string& stream, std::string& why)
{
	try
	{
		std::string h, b;
		if (!yamlutil::splitStream(stream, h, b))
		{
			why = "not a two-document stream";
			return false;
		}
		auto ht = yamlutil::parseMap(h);
		auto bt = yamlutil::parseMap(b);
		ryml::ConstNodeRef header = ht->crootref();
		ryml::ConstNodeRef body = bt->crootref();
		if (!yamlutil::getBool(header, "coop", false))
		{
			why = "host header missing coop:true";
			return false;
		}
		if (!yamlutil::hasKey(header, "coopPlayers"))
		{
			why = "host header missing coopPlayers";
			return false;
		}
		if (yamlutil::getInt(header, "saveSchema", 0) != SAVE_SCHEMA_CURRENT)
		{
			why = "host header saveSchema is not current";
			return false;
		}
		// Upgraded saves are always SEPARATE (SHARED postdates every schema-1 save).
		if (yamlutil::getInt(header, "coopCampaignType", -1) != 0)
		{
			why = "host header coopCampaignType is not SEPARATE (0)";
			return false;
		}
		if (yamlutil::getInt(body, "coop_save_owner_player_id", -1) != 0)
		{
			why = "host body owner id is not 0";
			return false;
		}
		if (!yamlutil::hasKey(body, "saveID"))
		{
			why = "host body missing saveID";
			return false;
		}
		ryml::ConstNodeRef seq = body.find_child("coopClientSaves");
		if (!seq.invalid())
		{
			for (ryml::ConstNodeRef e : seq.children())
			{
				ryml::ConstNodeRef blob = e.find_child("blob");
				if (blob.invalid() || !blob.has_val())
				{
					why = "embedded client blob missing";
					return false;
				}
				std::string cs = yamlutil::base64DecodeToString(blob.val());
				std::string ch, cb;
				if (!yamlutil::splitStream(cs, ch, cb))
				{
					why = "client blob is not a two-document stream";
					return false;
				}
				auto cht = yamlutil::parseMap(ch);
				auto cbt = yamlutil::parseMap(cb);
				if (yamlutil::getInt(cht->crootref(), "saveSchema", 0) != SAVE_SCHEMA_CURRENT)
				{
					why = "client header saveSchema is not current";
					return false;
				}
				if (yamlutil::getInt(cht->crootref(), "coopCampaignType", -1) != 0)
				{
					why = "client header coopCampaignType is not SEPARATE (0)";
					return false;
				}
				if (yamlutil::getInt(cbt->crootref(), "coop_save_owner_player_id", -1) != 1)
				{
					why = "client body owner id is not 1";
					return false;
				}
			}
		}
		// The detector must now classify the emitted stream as current.
		YAML::YamlRootNodeReader reader(YAML::YamlString{stream}, "<postflight>", false);
		DetectedSchema d2 = SchemaDetector::detectFromReaders(reader[0], reader[1].useIndex(), Options::getMasterUserFolder());
		if (d2.kind != DetectedSchema::Current)
		{
			why = "detector does not report current after upgrade";
			return false;
		}
		return true;
	}
	catch (const std::exception& e)
	{
		why = e.what();
		return false;
	}
}

std::string chooseBackupName(const std::string& fileName, int fromSchema)
{
	std::string base = CrossPlatform::noExt(fileName) + "_bak_v" + std::to_string(fromSchema);
	std::string candidate = base + ".sav";
	int n = 2;
	while (CrossPlatform::fileExists(Options::getMasterUserFolder() + candidate))
	{
		candidate = base + "-" + std::to_string(n) + ".sav";
		++n;
	}
	return candidate;
}

// Copy the original to the backup path, changing only the header "name" so the
// backup is distinguishable in the load list. Body is left byte-identical.
void writeBackupFile(const std::string& originalName, const std::string& backupName)
{
	std::string text = readFileText(originalName);
	std::string h, b;
	if (!yamlutil::splitStream(text, h, b))
	{
		// Could not split: fall back to a byte-for-byte copy.
		if (!CrossPlatform::copyFile(Options::getMasterUserFolder() + originalName,
									 Options::getMasterUserFolder() + backupName))
			throw Exception("failed to write backup " + backupName);
		return;
	}
	auto headerTree = yamlutil::parseMap(h);
	ryml::NodeRef header = headerTree->rootref();
	std::string origName = yamlutil::getStr(header, "name", CrossPlatform::noExt(originalName));
	yamlutil::setStr(header, "name", origName + " (pre-upgrade backup)");
	std::string out = yamlutil::emitNode(header);
	if (out.empty() || out.back() != '\n')
		out.push_back('\n');
	out += "---\n";
	out += b; // body byte-identical
	if (!CrossPlatform::writeFile(Options::getMasterUserFolder() + backupName, out))
		throw Exception("failed to write backup " + backupName);
}

////////////////////////////////////////////////////////////
//  Deep body scan for legacy co-op markers (detector v2)
////////////////////////////////////////////////////////////

// The distributed 1.7.0-era build wrote every soldier/vehicle/craft/base/ufo
// co-op key UNCONDITIONALLY, so their mere presence proves nothing. Only
// NON-DEFAULT values betray real co-op activity. We split them into two tiers:
//
//   STRONG - values that only appear once cross-instance co-op links were formed
//            (a soldier tied to a peer base/craft, a craft carrying peer items,
//            a target bound to a shared UFO/mission). These uniquely identify a
//            real co-op campaign -> classify Legacy/Dual, gate automatically.
//   WEAK   - keys the era serializer emitted for every save whether solo or not
//            (coop_gamemode, per-soldier coopname, a random base coopbaseid, the
//            debugCoopMenu option). Alone they cannot tell a co-op campaign from a
//            plain solo save made by a co-op-capable build -> AmbiguousBuild, ask.
//
// The relevant objects live at many depths (bases[].soldiers[], deadSoldiers[],
// bases[].crafts[], transfers[], top-level ufos[]/missionSites[], the options
// snapshot), so we walk the whole body tree and test each map node's own keys.
enum class MarkerLevel { None, Weak, Strong };

MarkerLevel scanReaderForMarkers(const YAML::YamlNodeReader& node, int depth)
{
	if (!node || depth > 64)
		return MarkerLevel::None;

	MarkerLevel best = MarkerLevel::None;

	if (node.isMap())
	{
		for (const YAML::YamlNodeReader& child : node.children())
		{
			std::string_view k = child.key();

			// --- STRONG: non-default cross-instance link values ---
			if (k == "coop")
			{
				if (child.readVal<int>(0) != 0)
					return MarkerLevel::Strong;
			}
			else if (k == "coopbase")
			{
				if (child.readVal<int>(-1) != -1)
					return MarkerLevel::Strong;
			}
			else if (k == "coopcraft")
			{
				if (child.readVal<int>(-1) != -1)
					return MarkerLevel::Strong;
			}
			else if (k == "coopcrafttype")
			{
				if (child.hasVal() && !child.val().empty())
					return MarkerLevel::Strong;
			}
			else if (k == "coopItems")
			{
				if (child.isSeq() && child.childrenCount() > 0)
					return MarkerLevel::Strong;
			}
			else if (k == "coopDestUfoId" || k == "coopDestMissionId"
				  || k == "coopUfoId" || k == "coopMissionId")
			{
				if (child.readVal<int>(0) != 0)
					return MarkerLevel::Strong;
			}
			// --- WEAK: keys the era serializer wrote unconditionally ---
			else if (k == "coop_gamemode" || k == "debugCoopMenu")
			{
				best = MarkerLevel::Weak;
			}
			else if (k == "coopname")
			{
				if (child.hasVal() && !child.val().empty())
					best = MarkerLevel::Weak;
			}
			else if (k == "coopbaseid")
			{
				if (child.readVal<int>(0) != 0)
					best = MarkerLevel::Weak;
			}

			// Recurse into nested containers (soldiers, crafts, options, ...).
			if (child.isMap() || child.isSeq())
			{
				MarkerLevel sub = scanReaderForMarkers(child, depth + 1);
				if (sub == MarkerLevel::Strong)
					return MarkerLevel::Strong;
				if (sub == MarkerLevel::Weak)
					best = MarkerLevel::Weak;
			}
		}
	}
	else if (node.isSeq())
	{
		for (const YAML::YamlNodeReader& child : node.children())
		{
			MarkerLevel sub = scanReaderForMarkers(child, depth + 1);
			if (sub == MarkerLevel::Strong)
				return MarkerLevel::Strong;
			if (sub == MarkerLevel::Weak)
				best = MarkerLevel::Weak;
		}
	}

	return best;
}

// Atomic write: temp then rename over the target (rename is the last step).
void atomicWriteFile(const std::string& fileName, const std::string& content)
{
	const std::string folder = Options::getMasterUserFolder();
	const std::string tmp = fileName + ".upgrade_tmp";
	if (!CrossPlatform::writeFile(folder + tmp, content))
		throw Exception("failed to write temp file " + tmp);
	if (!CrossPlatform::moveFile(folder + tmp, folder + fileName))
	{
		CrossPlatform::deleteFile(folder + tmp);
		throw Exception("failed to replace " + fileName);
	}
}

} // namespace (file-local)

////////////////////////////////////////////////////////////
//  SchemaDetector
////////////////////////////////////////////////////////////

DetectedSchema SchemaDetector::detectFromReaders(const YAML::YamlNodeReader& header,
												 const YAML::YamlNodeReader& body,
												 const std::string& masterUserFolder)
{
	DetectedSchema d;

	bool stampedLegacy = false;
	YAML::YamlNodeReader schemaNode = header["saveSchema"];
	if (schemaNode)
	{
		int s = schemaNode.readVal<int>(0);
		d.schema = s;
		if (s >= SAVE_SCHEMA_CURRENT)
		{
			d.kind = (s == SAVE_SCHEMA_CURRENT) ? DetectedSchema::Current : DetectedSchema::UnknownFuture;
			return d;
		}
		stampedLegacy = true; // s < CURRENT: an older stamped schema
	}
	else if (header["coop"].readVal(false))
	{
		// Unstamped host-authoritative save: current, re-stamped on next save.
		d.kind = DetectedSchema::Current;
		d.schema = SAVE_SCHEMA_CURRENT;
		return d;
	}

	// Body fingerprints for the schema-1 variants (order per PRD table).
	long long saveID = 0;
	body.tryRead("saveID", saveID);
	int gamemode = 0;
	body.tryRead("coop_gamemode", gamemode);
	bool hasEmbed = static_cast<bool>(body["coopClientSaveKey"]);

	if (hasEmbed)
	{
		d.kind = DetectedSchema::Legacy;
		d.schema = 1;
		d.variant = SchemaVariant::Embed;
		return d;
	}
	if (saveID != 0 && sidecarPresent(masterUserFolder, saveID))
	{
		d.kind = DetectedSchema::Legacy;
		d.schema = 1;
		d.variant = SchemaVariant::Sidecar;
		return d;
	}
	if (gamemode != 0 || saveID != 0)
	{
		d.kind = DetectedSchema::Legacy;
		d.schema = 1;
		d.variant = SchemaVariant::Dual;
		return d;
	}
	if (stampedLegacy)
	{
		// Stamped older schema with no recognizable variant: still legacy; let the
		// UI ask for a client save (dual path) rather than silently loading.
		d.kind = DetectedSchema::Legacy;
		d.variant = SchemaVariant::Dual;
		return d;
	}

	// Detector v2 (PRD 3, v2.1): the cheap header/top-level fingerprints above did
	// not fire, but the distributed 1.7.0-era build left co-op keys deep in the body
	// even when coop_gamemode/saveID were 0. Deep-scan the body:
	//   STRONG marker -> a real legacy co-op campaign (Legacy/Dual, auto-gate).
	//   WEAK-only     -> a save from a co-op-capable build that may be solo or co-op
	//                    (AmbiguousBuild: ask the player).
	//   neither       -> a genuine solo/vanilla OXCE save (loads untouched).
	MarkerLevel ml = scanReaderForMarkers(body, 0);
	if (ml == MarkerLevel::Strong)
	{
		d.kind = DetectedSchema::Legacy;
		d.schema = 1;
		d.variant = SchemaVariant::Dual;
		return d;
	}
	if (ml == MarkerLevel::Weak)
	{
		d.kind = DetectedSchema::AmbiguousBuild;
		d.schema = 1;
		d.variant = SchemaVariant::None; // resolved to Dual only if the player confirms co-op
		return d;
	}

	// No co-op trace at all: an ordinary solo save (gains the stamp on next save).
	d.kind = DetectedSchema::Solo;
	d.schema = SAVE_SCHEMA_CURRENT;
	return d;
}

DetectedSchema SchemaDetector::detect(const std::string& fileName)
{
	DetectedSchema d;
	std::string full = Options::getMasterUserFolder() + fileName;
	try
	{
		YAML::YamlRootNodeReader reader(full, false, false);
		YAML::YamlNodeReader header = reader[0];
		YAML::YamlNodeReader body = reader[1].useIndex();
		if (!header || !body)
		{
			d.kind = DetectedSchema::Malformed;
			return d;
		}
		return detectFromReaders(header, body, Options::getMasterUserFolder());
	}
	catch (...)
	{
		d.kind = DetectedSchema::Malformed;
		return d;
	}
}

////////////////////////////////////////////////////////////
//  Step registry
////////////////////////////////////////////////////////////

const std::vector<const SchemaStep*>& getSchemaSteps()
{
	static const std::vector<const SchemaStep*> steps = {
		step_1_to_2(),
	};
	return steps;
}

////////////////////////////////////////////////////////////
//  UpgradeRunner
////////////////////////////////////////////////////////////

UpgradeRunner::UpgradeRunner(const std::string& fileName) : _fileName(fileName)
{
}

DetectedSchema& UpgradeRunner::ensureDetected()
{
	if (!_detectedValid)
	{
		_detected = SchemaDetector::detect(_fileName);
		_detectedValid = true;
	}
	return _detected;
}

DetectedSchema UpgradeRunner::detect()
{
	return ensureDetected();
}

DetectedSchema UpgradeRunner::effectiveDetection(const UpgradeInputs& in)
{
	DetectedSchema d = ensureDetected();
	// The player answered "yes, it was a co-op campaign" in the ambiguous-build
	// dialog: an AmbiguousBuild save is a legacy dual save whose client world is a
	// separate standalone .sav, so route it through the exact 1->2 dual transform.
	if (d.kind == DetectedSchema::AmbiguousBuild && in.treatAmbiguousAsCoop)
	{
		d.kind = DetectedSchema::Legacy;
		d.schema = 1;
		d.variant = SchemaVariant::Dual;
	}
	return d;
}

std::vector<InputRequest> UpgradeRunner::requiredInputs()
{
	std::vector<InputRequest> reqs;
	DetectedSchema d = ensureDetected();
	if (!d.needsUpgrade())
		return reqs;

	// A minimal set carrying the detected variant is enough for a step to decide
	// its inputs; the client world (if any) is not needed to answer this.
	SaveSet probe;
	probe.schema = d.schema;
	probe.variant = d.variant;
	for (const SchemaStep* step : buildChain(d.schema))
	{
		std::vector<InputRequest> si = step->requiredInputs(probe);
		reqs.insert(reqs.end(), si.begin(), si.end());
	}
	return reqs;
}

PreflightResult UpgradeRunner::preflight(const UpgradeInputs& in)
{
	PreflightResult r;
	DetectedSchema d = effectiveDetection(in);

	if (d.kind == DetectedSchema::Malformed)
	{
		r.errors.push_back("Save file could not be parsed.");
		return r;
	}
	if (d.kind == DetectedSchema::UnknownFuture)
	{
		r.errors.push_back("Save was made by a newer version (schema " + std::to_string(d.schema) + ") and cannot be upgraded by this build.");
		return r;
	}
	if (!d.needsUpgrade())
		return r; // current / solo: nothing to do

	std::vector<const SchemaStep*> chain = buildChain(d.schema);
	if (chain.empty() || chain.back()->toSchema() != SAVE_SCHEMA_CURRENT)
	{
		r.errors.push_back("This build cannot upgrade schema " + std::to_string(d.schema) + ".");
		return r;
	}

	try
	{
		SaveSet set;
		ingestSaveSet(set, d, in, _fileName);
		for (const SchemaStep* step : chain)
		{
			step->validate(set, in, r);
			if (!r.ok())
				break;
			step->apply(set, in); // advance so later steps validate the right shape
		}
	}
	catch (const std::exception& e)
	{
		r.errors.push_back(std::string("Upgrade preflight failed: ") + e.what());
	}
	return r;
}

ExecuteResult UpgradeRunner::execute(const UpgradeInputs& in)
{
	ExecuteResult res;
	DetectedSchema d = effectiveDetection(in);

	if (!d.needsUpgrade())
	{
		res.errorMessage = "Save does not need upgrading.";
		return res;
	}
	std::vector<const SchemaStep*> chain = buildChain(d.schema);
	if (chain.empty() || chain.back()->toSchema() != SAVE_SCHEMA_CURRENT)
	{
		res.errorMessage = "This build cannot upgrade schema " + std::to_string(d.schema) + ".";
		return res;
	}

	try
	{
		// 1. Validate the whole chain on a throwaway copy (no writes yet).
		{
			PreflightResult pf;
			SaveSet check;
			ingestSaveSet(check, d, in, _fileName);
			for (const SchemaStep* step : chain)
			{
				step->validate(check, in, pf);
				if (!pf.ok())
					break;
				step->apply(check, in);
			}
			if (!pf.ok())
			{
				res.errorMessage = "Upgrade blocked: " + joinLines(pf.errors, "; ");
				return res;
			}
		}

		// 2. Back up the original first (does not touch the original file).
		std::string backupName = chooseBackupName(_fileName, d.schema);
		writeBackupFile(_fileName, backupName);
		res.backupPath = Options::getMasterUserFolder() + backupName;
		res.reportLines.push_back("Backup written: " + backupName);

		// 3. Run the migration chain in memory.
		SaveSet set;
		ingestSaveSet(set, d, in, _fileName);
		for (const SchemaStep* step : chain)
			step->apply(set, in);
		res.reportLines.insert(res.reportLines.end(), set.report.begin(), set.report.end());

		// 4. Emit + post-flight verify (still in memory - original intact).
		std::string upgraded = emitHostStream(set);
		std::string why;
		if (!postflightVerify(upgraded, why))
		{
			res.errorMessage = "Post-flight verification failed: " + why + " (original untouched)";
			return res;
		}

		// 5. Atomic write over the original (last step).
		atomicWriteFile(_fileName, upgraded);
		res.reportLines.push_back("Upgraded save written: " + _fileName);
		res.success = true;
	}
	catch (const std::exception& e)
	{
		res.errorMessage = std::string("Upgrade failed: ") + e.what();
		res.success = false;
	}

	if (res.success)
		Log(LOG_INFO) << "[save-upgrade] " << _fileName << " upgraded (backup: " << res.backupPath << ")";
	else
		Log(LOG_ERROR) << "[save-upgrade] " << _fileName << " failed: " << res.errorMessage;
	return res;
}

////////////////////////////////////////////////////////////
//  Debug self-test (disk-less)
////////////////////////////////////////////////////////////

bool runSelfTest(std::vector<std::string>& log)
{
	YAML::setGlobalErrorHandler();

	static const char* HOST_FIXTURE =
		"name: TestCoop\n"
		"version: Extended 8.4.2\n"
		"time:\n"
		"  year: 1999\n"
		"  month: 1\n"
		"  day: 1\n"
		"  hour: 12\n"
		"  minute: 0\n"
		"  second: 0\n"
		"  weekday: 6\n"
		"mods:\n"
		"  - xcom1 ver: 1.0\n"
		"---\n"
		"difficulty: 0\n"
		"coop_gamemode: 0\n"
		"saveID: 12340000\n"
		"coopClientSaveKey: host_12340000_Bob.data\n"
		"bases:\n"
		"  - name: Alpha\n"
		"    soldiers:\n"
		"      - type: STR_SOLDIER\n"
		"        id: 1\n"
		"        name: Alice\n"
		"        coopname: \"\"\n";

	static const char* CLIENT_FIXTURE =
		"name: TestCoop-client\n"
		"version: Extended 8.4.2\n"
		"time:\n"
		"  year: 1999\n"
		"  month: 1\n"
		"  day: 1\n"
		"  hour: 12\n"
		"  minute: 0\n"
		"  second: 0\n"
		"  weekday: 6\n"
		"mods:\n"
		"  - xcom1 ver: 1.0\n"
		"---\n"
		"difficulty: 0\n"
		"coop_gamemode: 0\n"
		"saveID: 12340000\n"
		"bases:\n"
		"  - name: BobBase\n"
		"    soldiers:\n"
		"      - type: STR_SOLDIER\n"
		"        id: 5\n"
		"        name: Bob\n";

	try
	{
		// 1. Detector recognizes the schema-1 embed host.
		{
			YAML::YamlRootNodeReader r(YAML::YamlString{std::string(HOST_FIXTURE)}, "<selftest-host>", false);
			DetectedSchema d = SchemaDetector::detectFromReaders(r[0], r[1].useIndex(), std::string());
			if (d.kind != DetectedSchema::Legacy || d.variant != SchemaVariant::Embed)
			{
				log.push_back("FAIL: detector did not classify schema-1 embed");
				return false;
			}
			log.push_back("OK: detector classified schema-1 embed");
		}

		// 2. Run the 1->2 transform directly on a SaveSet.
		SaveSet set;
		set.schema = 1;
		set.variant = SchemaVariant::Embed;
		set.host.ingestStream(std::string(HOST_FIXTURE));
		set.saveID = yamlutil::getInt(set.host.body(), "saveID", 0);
		set.gamemode = static_cast<int>(yamlutil::getInt(set.host.body(), "coop_gamemode", 0));
		{
			ClientWorld cw;
			cw.name = "Bob";
			cw.world.ingestStream(std::string(CLIENT_FIXTURE));
			set.clients.push_back(std::move(cw));
		}
		UpgradeInputs in;
		in.hostName = "";
		in.clientName = "Bob";
		step_1_to_2()->apply(set, in);

		if (!yamlutil::getBool(set.host.header(), "coop", false))
		{
			log.push_back("FAIL: host header coop not set");
			return false;
		}
		if (yamlutil::getInt(set.host.header(), "saveSchema", 0) != SAVE_SCHEMA_CURRENT)
		{
			log.push_back("FAIL: host header saveSchema not current");
			return false;
		}
		if (yamlutil::getInt(set.host.body(), "coop_save_owner_player_id", -1) != 0)
		{
			log.push_back("FAIL: host owner id not 0");
			return false;
		}
		if (yamlutil::hasKey(set.host.body(), "coopClientSaveKey"))
		{
			log.push_back("FAIL: coopClientSaveKey not removed");
			return false;
		}
		if (set.roster.size() != 2 || set.roster[0] != "" || set.roster[1] != "Bob")
		{
			log.push_back("FAIL: roster not [\"\", \"Bob\"]");
			return false;
		}
		if (set.saveID < 10000000000000LL)
		{
			log.push_back("FAIL: saveID not minted as YYYYMMDDHHMMSS");
			return false;
		}
		// host soldier tagged + coopname filled
		{
			ryml::ConstNodeRef s = set.host.body().find_child("bases").child(0).find_child("soldiers").child(0);
			if (yamlutil::getInt(s, "ownerplayerid", -1) != 0)
			{
				log.push_back("FAIL: host soldier ownerplayerid not 0");
				return false;
			}
			if (yamlutil::getStr(s, "coopname", "") != "Alice")
			{
				log.push_back("FAIL: host soldier coopname not filled");
				return false;
			}
		}
		// client tagged owner 1
		if (set.clients.size() != 1 || yamlutil::getInt(set.clients[0].world.body(), "coop_save_owner_player_id", -1) != 1)
		{
			log.push_back("FAIL: client owner id not 1");
			return false;
		}
		log.push_back("OK: 1->2 transform tagged host/client and minted identity");

		// 3. Emit + post-flight verify (blob round-trips, detector reports current).
		std::string emitted = emitHostStream(set);
		std::string why;
		if (!postflightVerify(emitted, why))
		{
			log.push_back("FAIL: post-flight - " + why);
			return false;
		}
		log.push_back("OK: emit + post-flight verified");
		return true;
	}
	catch (const std::exception& e)
	{
		log.push_back(std::string("EXCEPTION: ") + e.what());
		return false;
	}
}

} // namespace SaveUpgrade

} // namespace OpenXcom
