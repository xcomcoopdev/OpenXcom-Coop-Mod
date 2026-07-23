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

// In-game Save Upgrader - PUBLIC API (Phase A: core engine).
//
// This header is the surface the load-gate / dialog / picker UI (Phase B) drives.
// It intentionally does NOT expose rapidyaml: the transform internals live in
// SaveUpgradeTypes.h, which only the upgrade .cpp files include.
//
// Typical UI flow:
//   UpgradeRunner runner(fileName);
//   DetectedSchema d = runner.detect();
//   if (d.needsUpgrade()) {
//       auto inputs = runner.requiredInputs();        // ask UI to fill these
//       UpgradeInputs collected = ...;                // from dialog/picker
//       PreflightResult pf = runner.preflight(collected);
//       if (pf.ok() && userConfirmed) {
//           ExecuteResult r = runner.execute(collected);   // backs up + writes
//       }
//   }

#include <string>
#include <vector>

#include "../../version.h" // SAVE_SCHEMA_CURRENT

namespace OpenXcom
{

namespace YAML
{
class YamlNodeReader;
}

namespace SaveUpgrade
{

/// Which schema-1 sub-variant a legacy save is - i.e. WHERE the client world
/// comes from. All three feed the same 1->2 transform; only ingestion differs.
enum class SchemaVariant
{
	None,    // not a legacy variant (current / solo)
	Embed,   // client world base64-embedded in the host body (coopClientSaveBlob)
	Sidecar, // client world on host disk as host_<saveID>_<name>.data files
	Dual     // client world is a separate standalone .sav on the client's machine
};

/// Result of classifying a save file cheaply, before any SavedGame construction.
struct DetectedSchema
{
	enum Kind
	{
		Current,        // schema == SAVE_SCHEMA_CURRENT (stamped, or unstamped coop:true)
		Solo,           // ordinary single-player save, no co-op trace - loads normally
		Legacy,         // an older co-op schema that must be upgraded to play
		UnknownFuture,  // saveSchema > SAVE_SCHEMA_CURRENT (made by a newer build)
		Malformed       // could not parse as a two-document save
	};

	Kind kind = Solo;
	int schema = SAVE_SCHEMA_CURRENT;              // detected schema number
	SchemaVariant variant = SchemaVariant::None;   // meaningful when kind == Legacy

	/// True when this build can upgrade the save straight away (a chain exists and
	/// the variant is known). Only a positively-detected legacy co-op save ever
	/// gates; solo saves are never touched (detector v2.3).
	bool needsUpgrade() const { return kind == Legacy; }
	/// True when the save cannot be loaded OR upgraded by this build.
	bool isBlocking() const { return kind == UnknownFuture || kind == Malformed; }
	/// True when the save can be handed straight to SavedGame::load with no gate.
	bool loadsDirectly() const { return kind == Current || kind == Solo; }
};

/// Classifies save files. Callable cheaply from load paths (header-first probe).
class SchemaDetector
{
public:
	/// Reads fileName from the master user folder and classifies it.
	static DetectedSchema detect(const std::string& fileName);

	/// Classifies from already-parsed header/body readers (avoids re-parsing on
	/// the load path). masterUserFolder is used only for the sidecar probe.
	static DetectedSchema detectFromReaders(const YAML::YamlNodeReader& header,
											const YAML::YamlNodeReader& body,
											const std::string& masterUserFolder);
};

/// An input the UI must collect before an upgrade can run (dual variant only today).
struct InputRequest
{
	enum Type
	{
		ClientSaveFile, // pick the other player's .sav from the saves folder
		ClientName,     // roster[1] - must exactly match the connect name
		HostName        // roster[0] - optional, blank claimed at re-host
	};

	Type type;
	bool required;
	std::string id;     // stable identifier
	std::string prompt; // human hint / STR key suggestion for Phase B
};

/// The values the UI collects for the requested inputs.
struct UpgradeInputs
{
	std::string clientSaveFileName; // dual: filename of the picked client .sav
	std::string clientName;         // roster[1]
	std::string hostName;           // roster[0] (blank => claimed at next host)
	bool skipClient = false;        // dual escape hatch: proceed with no client world
};

/// Blocking errors and non-blocking warnings from a preflight check.
struct PreflightResult
{
	std::vector<std::string> errors;   // blocking - upgrade must not run
	std::vector<std::string> warnings; // non-blocking (some warrant an explicit confirm)

	bool ok() const { return errors.empty(); }
};

/// Outcome of an execute(). On failure the original file is untouched.
struct ExecuteResult
{
	bool success = false;
	std::string upgradedPath;            // full path of the NEW upgraded file (original untouched)
	std::vector<std::string> reportLines; // human-readable, shown in summary + logged
	std::string errorMessage;            // set when success == false
};

/// Drives detect -> requiredInputs -> preflight -> execute for one save file.
/// Steps operate on raw YAML document trees; a SavedGame object is never built.
class UpgradeRunner
{
public:
	/// fileName is relative to Options::getMasterUserFolder().
	explicit UpgradeRunner(const std::string& fileName);

	const std::string& fileName() const { return _fileName; }

	/// Classify the save (cached after first call).
	DetectedSchema detect();

	/// The inputs the UI must collect before preflight/execute can run.
	std::vector<InputRequest> requiredInputs();

	/// Ingest + validate without writing anything. Safe to call repeatedly.
	PreflightResult preflight(const UpgradeInputs& in);

	/// Backup the original, run the migration chain in memory, then atomically
	/// replace the original with the upgraded save. The write is the last step,
	/// so any earlier failure leaves the original intact.
	ExecuteResult execute(const UpgradeInputs& in);

private:
	std::string _fileName;
	DetectedSchema _detected;
	bool _detectedValid = false;

	DetectedSchema& ensureDetected();
};

/// Debug self-test: builds a synthetic schema-1 "embed" save pair in memory,
/// runs the detector and the 1->2 transform, and verifies the result. Touches
/// no disk. Returns true on pass; appends human-readable steps to log.
bool runSelfTest(std::vector<std::string>& log);

} // namespace SaveUpgrade

} // namespace OpenXcom
