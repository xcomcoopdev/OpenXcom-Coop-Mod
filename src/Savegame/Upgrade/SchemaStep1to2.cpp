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

// Schema step 1 -> 2 : legacy co-op save (schema 1, any variant) to the current
// host-authoritative single-save format (schema 2). See PRD save-upgrader.md 7.
//
// This is a frozen step: a future format change adds a new file, it never edits
// this one. Ingestion (WHERE the client world comes from) is the framework's job
// (see SaveUpgrade.cpp); this step is a pure field transform on the decoded docs.

#include "SaveUpgradeTypes.h"

#include <cstdlib>
#include <ctime>
#include <set>
#include <string>

namespace OpenXcom
{

namespace SaveUpgrade
{

namespace
{

using yamlutil::getBool;
using yamlutil::getInt;
using yamlutil::getStr;
using yamlutil::hasKey;
using yamlutil::mapChild;
using yamlutil::removeKey;
using yamlutil::setBool;
using yamlutil::setInt;
using yamlutil::setStr;

// Mint a save identity, YYYYMMDDHHMMSS as an int (equal-width; blob-key ordering
// relies on it). Mirrors connectionTCP::getDateTimeCoop exactly.
long long mintSaveID()
{
	std::time_t now = std::time(nullptr);
	std::tm lt = {};
#ifdef _WIN32
	localtime_s(&lt, &now);
#else
	localtime_r(&now, &lt);
#endif
	return static_cast<long long>(lt.tm_year + 1900) * 10000000000LL
		 + static_cast<long long>(lt.tm_mon + 1) * 100000000LL
		 + static_cast<long long>(lt.tm_mday) * 1000000LL
		 + static_cast<long long>(lt.tm_hour) * 10000LL
		 + static_cast<long long>(lt.tm_min) * 100LL
		 + static_cast<long long>(lt.tm_sec);
}

// The name that becomes roster[1] / the client blob key.
std::string effectiveClientName(const SaveSet& set, const UpgradeInputs& in)
{
	if (set.variant == SchemaVariant::Dual)
		return in.skipClient ? std::string() : in.clientName;
	if (!set.clients.empty())
		return set.clients[0].name; // embed/sidecar: derived during ingest
	return in.clientName;
}

// A player name must be non-empty, printable, and safe as a blob-key filename.
bool validPlayerName(const std::string& n)
{
	if (n.empty())
		return false;
	for (unsigned char c : n)
	{
		if (c < 0x20)
			return false; // control chars
		if (c == '/' || c == '\\')
			return false; // path separators (the blob key is a filename)
	}
	return true;
}

int countBases(ryml::ConstNodeRef body)
{
	ryml::ConstNodeRef b = body.find_child("bases");
	if (b.invalid() || !b.is_seq())
		return 0;
	return static_cast<int>(b.num_children());
}

// Walk every base's soldiers: tag ownerplayerid and fill an empty/missing
// coopname with the soldier's name (the engine ctor rule, Soldier.cpp:153).
int tagSoldiers(ryml::NodeRef body, int ownerId)
{
	int count = 0;
	ryml::NodeRef bases = body.find_child("bases");
	if (bases.invalid() || !bases.is_seq())
		return 0;
	for (ryml::NodeRef base : bases.children())
	{
		ryml::NodeRef soldiers = base.find_child("soldiers");
		if (soldiers.invalid() || !soldiers.is_seq())
			continue;
		for (ryml::NodeRef s : soldiers.children())
		{
			setInt(s, "ownerplayerid", ownerId);
			std::string cn = getStr(s, "coopname", std::string());
			if (cn.empty())
			{
				std::string nm = getStr(s, "name", std::string());
				if (!nm.empty())
					setStr(s, "coopname", nm);
			}
			++count;
		}
	}
	return count;
}

void writeRoster(ryml::NodeRef header, const std::vector<std::string>& roster)
{
	removeKey(header, "coopPlayers");
	ryml::NodeRef seq = mapChild(header, "coopPlayers");
	seq |= ryml::SEQ;
	for (const std::string& p : roster)
	{
		// set_val_serialized copies the name into the tree arena and stores it as
		// a scalar - identical to how the engine writes coopPlayers (an empty host
		// name becomes an empty scalar that reads back as ""), so it round-trips.
		seq.append_child().set_val_serialized(p);
	}
}

std::vector<std::string> readSeqStrings(ryml::ConstNodeRef map, ryml::csubstr key)
{
	std::vector<std::string> out;
	if (map.invalid() || !map.is_map())
		return out;
	ryml::ConstNodeRef n = map.find_child(key);
	if (n.invalid() || !n.is_seq())
		return out;
	for (ryml::ConstNodeRef c : n.children())
	{
		if (c.has_val())
		{
			ryml::csubstr v = c.val();
			out.emplace_back(v.str, v.len);
		}
	}
	return out;
}

// Rough absolute in-game hour count for a header's time map. Not calendar-exact
// (31-day months) - only used to order two saves for the divergence warning.
long long absHours(ryml::ConstNodeRef header)
{
	ryml::ConstNodeRef t = header.find_child("time");
	if (t.invalid())
		return 0;
	long long y = getInt(t, "year", 0);
	long long mo = getInt(t, "month", 1);
	long long da = getInt(t, "day", 1);
	long long ho = getInt(t, "hour", 0);
	return (((y * 12 + mo) * 31 + da) * 24 + ho);
}

std::vector<long long> collectUfoIds(ryml::ConstNodeRef body)
{
	std::vector<long long> ids;
	ryml::ConstNodeRef u = body.find_child("ufos");
	if (!u.invalid() && u.is_seq())
	{
		for (ryml::ConstNodeRef e : u.children())
		{
			long long id = getInt(e, "id", -1);
			if (id != -1)
				ids.push_back(id);
		}
	}
	return ids;
}

// Counts of stale session-scoped links reset during a 1->2 transform (PRD 7,
// v2.1). Every field here referenced a dead session's RANDOM ids; post-upgrade
// ownership is carried by ownerplayerid, so leaving them would resurrect broken
// cross-instance links. coopname is deliberately NOT reset (it is real identity).
struct LinkResetCounts
{
	int soldierVehicle = 0; // coop / coopbase / coopcraft / coopcrafttype cleared on a soldier or vehicle
	int craftItems = 0;     // coopItems peer-item cache removed from a craft
	int craftDest = 0;      // coopDestUfoId / coopDestMissionId cleared on a craft
	int ufoMission = 0;     // coopUfoId / coopMissionId cleared on a ufo or mission site
};

// Walk the whole body tree and reset every stale co-op link field in place. The
// same objects live at many depths (bases[].soldiers[], deadSoldiers[],
// bases[].crafts[], transfers[], top-level ufos[]/missionSites[]), so we test
// each map node's own keys rather than assuming a fixed path.
void resetStaleLinks(ryml::NodeRef node, LinkResetCounts& c, int depth)
{
	if (node.invalid() || depth > 64)
		return;

	if (node.is_map())
	{
		// soldier / vehicle session links -> defaults (keep coopname).
		bool touchedSV = false;
		if (hasKey(node, "coop") && getInt(node, "coop", 0) != 0)
		{
			setInt(node, "coop", 0);
			touchedSV = true;
		}
		if (hasKey(node, "coopbase") && getInt(node, "coopbase", -1) != -1)
		{
			setInt(node, "coopbase", -1);
			touchedSV = true;
		}
		if (hasKey(node, "coopcraft") && getInt(node, "coopcraft", -1) != -1)
		{
			setInt(node, "coopcraft", -1);
			touchedSV = true;
		}
		if (hasKey(node, "coopcrafttype") && !getStr(node, "coopcrafttype", std::string()).empty())
		{
			setStr(node, "coopcrafttype", std::string());
			touchedSV = true;
		}
		if (touchedSV)
			++c.soldierVehicle;

		// craft peer-item cache -> gone.
		if (hasKey(node, "coopItems"))
		{
			removeKey(node, "coopItems");
			++c.craftItems;
		}

		// craft cross-instance destination -> zeroed.
		bool touchedDest = false;
		if (hasKey(node, "coopDestUfoId") && getInt(node, "coopDestUfoId", 0) != 0)
		{
			setInt(node, "coopDestUfoId", 0);
			touchedDest = true;
		}
		if (hasKey(node, "coopDestMissionId") && getInt(node, "coopDestMissionId", 0) != 0)
		{
			setInt(node, "coopDestMissionId", 0);
			touchedDest = true;
		}
		if (touchedDest)
			++c.craftDest;

		// ufo / mission cross-instance id -> zeroed.
		bool touchedUM = false;
		if (hasKey(node, "coopUfoId") && getInt(node, "coopUfoId", 0) != 0)
		{
			setInt(node, "coopUfoId", 0);
			touchedUM = true;
		}
		if (hasKey(node, "coopMissionId") && getInt(node, "coopMissionId", 0) != 0)
		{
			setInt(node, "coopMissionId", 0);
			touchedUM = true;
		}
		if (touchedUM)
			++c.ufoMission;

		for (ryml::NodeRef ch : node.children())
			resetStaleLinks(ch, c, depth + 1);
	}
	else if (node.is_seq())
	{
		for (ryml::NodeRef ch : node.children())
			resetStaleLinks(ch, c, depth + 1);
	}
}

void collectCoopnames(ryml::ConstNodeRef body, std::vector<std::string>& names)
{
	ryml::ConstNodeRef bases = body.find_child("bases");
	if (bases.invalid() || !bases.is_seq())
		return;
	for (ryml::ConstNodeRef base : bases.children())
	{
		ryml::ConstNodeRef soldiers = base.find_child("soldiers");
		if (soldiers.invalid() || !soldiers.is_seq())
			continue;
		for (ryml::ConstNodeRef s : soldiers.children())
		{
			std::string cn = getStr(s, "coopname", std::string());
			if (cn.empty())
				cn = getStr(s, "name", std::string());
			if (!cn.empty())
				names.push_back(cn);
		}
	}
}

class Step_1_to_2 : public SchemaStep
{
public:
	int fromSchema() const override { return 1; }
	int toSchema() const override { return 2; }

	std::vector<InputRequest> requiredInputs(const SaveSet& set) const override
	{
		std::vector<InputRequest> reqs;
		if (set.variant == SchemaVariant::Dual)
		{
			reqs.push_back({InputRequest::ClientSaveFile, true, "clientSaveFile",
							"Copy the other player's save file into this game's saves folder first, then select it here."});
			reqs.push_back({InputRequest::ClientName, true, "clientName",
							"The client player's name - must exactly match the name that player connects with."});
			reqs.push_back({InputRequest::HostName, false, "hostName",
							"Your (host) player name - optional; leave blank to claim it automatically at the next host."});
		}
		// embed / sidecar: none (the framework auto-ingests the client world).
		return reqs;
	}

	void validate(const SaveSet& set, const UpgradeInputs& in, PreflightResult& out) const override
	{
		if (!set.host.valid())
		{
			out.errors.push_back("The host save is not a valid two-document YAML stream.");
			return;
		}
		ryml::ConstNodeRef hb = set.host.body();

		// mid-battle saves are refused (§7).
		if (hasKey(hb, "battleGame"))
			out.errors.push_back("This is a mid-battle save. Finish the mission and save from the Geoscape using the old version first, then upgrade.");

		// zero host bases is a blocking error.
		if (countBases(hb) == 0)
			out.errors.push_back("The host save has no bases; it cannot anchor a co-op campaign.");

		const bool skip = (set.variant == SchemaVariant::Dual && in.skipClient);
		const bool haveClient = !set.clients.empty();
		const std::string cname = effectiveClientName(set, in);

		if (!skip)
		{
			if (cname.empty())
				out.errors.push_back("A client player name is required.");
			else if (!validPlayerName(cname))
				out.errors.push_back("The client player name contains characters that are not allowed.");
		}

		if (skip || !haveClient)
			out.warnings.push_back("No client world will be included; that player will restart fresh when they rejoin.");

		for (const ClientWorld& c : set.clients)
		{
			ryml::ConstNodeRef cb = c.world.body();
			if (hasKey(cb, "battleGame"))
				out.errors.push_back("The client save is a mid-battle save; finish or discard that battle first.");

			int cgm = static_cast<int>(getInt(cb, "coop_gamemode", set.gamemode));
			if (cgm != set.gamemode)
				out.errors.push_back("The co-op game mode differs between the host and client saves.");

			if (readSeqStrings(set.host.header(), "mods") != readSeqStrings(c.world.header(), "mods"))
				out.warnings.push_back("The mod lists differ between the host and client saves.");

			long long dh = std::llabs(absHours(set.host.header()) - absHours(c.world.header()));
			if (dh > 24)
				out.warnings.push_back("The host and client saves are more than 24 in-game hours apart; the client world may resurrect rolled-back state.");
			else if (dh > 0)
				out.warnings.push_back("The host and client save times differ.");
		}

		// duplicate coopname after fill (warning only).
		std::vector<std::string> names;
		collectCoopnames(hb, names); // host body
		for (const ClientWorld& c : set.clients)
			collectCoopnames(c.world.body(), names);
		std::set<std::string> seen, dup;
		for (const std::string& n : names)
		{
			if (!seen.insert(n).second)
				dup.insert(n);
		}
		for (const std::string& n : dup)
			out.warnings.push_back("Duplicate soldier co-op name after fill: " + n);
	}

	void apply(SaveSet& set, const UpgradeInputs& in) const override
	{
		long long newID = mintSaveID();
		set.saveID = newID;
		std::string clientName = effectiveClientName(set, in);
		set.roster.clear();
		set.roster.push_back(in.hostName);
		set.roster.push_back(clientName);

		// --- host doc ---
		ryml::NodeRef hh = set.host.header();
		setBool(hh, "coop", true);
		setInt(hh, "saveSchema", SAVE_SCHEMA_CURRENT);
		writeRoster(hh, set.roster);

		ryml::NodeRef hb = set.host.body();
		setInt(hb, "saveID", newID);
		setInt(hb, "coop_save_owner_player_id", 0);
		setBool(hb, "no_bases", false);
		if (!hasKey(hb, "coop_gamemode"))
			setInt(hb, "coop_gamemode", set.gamemode);
		// embed-variant leftovers, removed after ingestion.
		removeKey(hb, "coopClientSaveKey");
		removeKey(hb, "coopClientSaveBlob");
		int hostBases = countBases(hb);
		int hostSoldiers = tagSoldiers(hb, 0);

		// Reset stale session-scoped links across the host world (PRD 7, v2.1).
		LinkResetCounts resets;
		resetStaleLinks(hb, resets, 0);

		// --- client doc(s) ---
		int clientBases = 0, clientSoldiers = 0;
		for (ClientWorld& c : set.clients)
		{
			ryml::NodeRef ch = c.world.header();
			ryml::NodeRef cb = c.world.body();
			setBool(ch, "coop", true);
			setInt(ch, "saveSchema", SAVE_SCHEMA_CURRENT);
			writeRoster(ch, set.roster);

			setInt(cb, "saveID", newID);
			setInt(cb, "coop_save_owner_player_id", 1);
			int cbases = countBases(cb);
			setBool(cb, "no_bases", cbases == 0);
			if (!hasKey(cb, "coop_gamemode"))
				setInt(cb, "coop_gamemode", set.gamemode);
			clientBases += cbases;
			clientSoldiers += tagSoldiers(cb, 1);
			// ...and across every client world too.
			resetStaleLinks(cb, resets, 0);
		}
		// A dual client is keyed by the collected roster name so emit embeds it
		// under host_<saveID>_<clientName>.data.
		if (set.variant == SchemaVariant::Dual && set.clients.size() == 1)
			set.clients[0].name = clientName;

		// --- report ---
		set.report.push_back("Minted saveID " + std::to_string(newID) + ".");
		set.report.push_back("Roster: host='" + set.roster[0] + "', client='" + set.roster[1] + "'.");
		set.report.push_back("Co-op game mode: " + std::to_string(set.gamemode) + " (PVP/PVE variants are untested - please report issues).");
		set.report.push_back("Host: " + std::to_string(hostBases) + " base(s), " + std::to_string(hostSoldiers) + " soldier(s) tagged owner 0.");
		if (set.clients.empty())
			set.report.push_back("No client world embedded (skip path); that player restarts fresh on rejoin.");
		else
			set.report.push_back("Client '" + clientName + "': " + std::to_string(clientBases) + " base(s), " + std::to_string(clientSoldiers) + " soldier(s) tagged owner 1.");

		// stale session-link resets (report each class with counts, PRD 7 v2.1).
		if (resets.soldierVehicle || resets.craftItems || resets.craftDest || resets.ufoMission)
		{
			set.report.push_back("Reset stale co-op links from the dead session:");
			set.report.push_back("  soldier/vehicle base+craft links cleared: " + std::to_string(resets.soldierVehicle) + ".");
			set.report.push_back("  craft peer-item caches removed: " + std::to_string(resets.craftItems) + ".");
			set.report.push_back("  craft cross-instance destinations cleared: " + std::to_string(resets.craftDest) + ".");
			set.report.push_back("  ufo/mission cross-instance ids cleared: " + std::to_string(resets.ufoMission) + ".");
		}
		else
		{
			set.report.push_back("No stale co-op links to reset.");
		}

		// contamination (report only, PRD §7): duplicate UFO ids across worlds.
		std::vector<long long> hostIds = collectUfoIds(hb);
		std::set<long long> hs(hostIds.begin(), hostIds.end());
		for (const ClientWorld& c : set.clients)
		{
			for (long long id : collectUfoIds(c.world.body()))
			{
				if (hs.count(id))
					set.report.push_back("Contamination note: UFO id " + std::to_string(id) + " appears in both host and client worlds (schema-1 sync-bug leakage; not stripped).");
			}
		}
	}
};

} // namespace

const SchemaStep* step_1_to_2()
{
	// Meyers singleton: constructed on first use, so there is no cross-TU static
	// initialization-order dependency with the registry in SaveUpgrade.cpp.
	static const Step_1_to_2 instance;
	return &instance;
}

} // namespace SaveUpgrade

} // namespace OpenXcom
