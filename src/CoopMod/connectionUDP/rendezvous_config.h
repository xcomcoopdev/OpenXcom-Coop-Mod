#pragma once

/*
 * Runtime rendezvous configuration for the OpenXcom UDP transport.
 *
 * Multiple named rendezvous servers are read once, lazily, from a JSON file so a
 * deployment/config can be passed around without committing it to source control.
 * The Server Browser lets the user pick which one is "active"; all coop operations
 * (list/host/join/direct/add) use the active server.
 *
 * File: rendezvous.json
 *   {
 *     "servers": [
 *       {
 *         "name": "Official",
 *         "host": "<ip or hostname>",
 *         "tcpPort": <tcp port>,
 *         "udpPort": <udp port>,
 *         "gameVersion": "<version string>",
 *         "serverBoxPublicKey": "<base64 crypto_box public key>",
 *         "serverSignPublicKey": "<base64 crypto_sign public key>"
 *       }
 *     ]
 *   }
 *
 * Search order (first file that exists wins):
 *   1. path in env var OXC_RENDEZVOUS_CONFIG
 *   2. <exe folder>/rendezvous.json
 *   3. <config folder>/rendezvous.json
 *
 * SECURITY:
 *  - Only PUBLIC keys belong in this file.
 *  - Never put server_box_secret.key or server_sign_secret.key in the client.
 */

#include "rendezvous_client.h"

#include <cstdint>
#include <string>
#include <vector>

namespace OpenXcom
{

struct BuiltInRendezvousConfig
{
    std::string host;
    uint16_t tcpPort = 0;
    uint16_t udpPort = 0;
    std::string gameVersion;
};

// --- Server list / selection -------------------------------------------------

/// Names of all configured rendezvous servers, in file order.
std::vector<std::string> getRendezvousServerNames();

/// Number of configured rendezvous servers (0 if none / file missing).
size_t getRendezvousServerCount();

/// Index of the currently active server (0 if none configured).
size_t getActiveRendezvousServer();

/// Name of the currently active server ("" if none configured).
std::string getActiveRendezvousServerName();

/// Selects the active server by index. Out-of-range is clamped to a valid index
/// (or ignored if no servers are configured).
void setActiveRendezvousServer(size_t index);

/// Selects the active server by name. Any unknown/empty name falls back to index 0.
void setActiveRendezvousServerByName(const std::string& name);

/// Host/ports/version + decoded keys for an arbitrary server index, without
/// changing the active selection (used for offline probing). Returns false and
/// sets *error if the index is invalid or the keys are absent/invalid.
bool getRendezvousServerConfig(size_t index,
                               BuiltInRendezvousConfig& outCfg,
                               RendezvousClient::ServerKeys& outKeys,
                               std::string* error = nullptr);

// --- Active-server accessors (unchanged signatures) --------------------------

/// Host/ports/version of the ACTIVE server (empty if not configured).
BuiltInRendezvousConfig getBuiltInRendezvousConfig();

/// Decoded public keys of the ACTIVE server.
/// Returns false (and sets *error) if not configured or the keys are invalid.
bool loadBuiltInRendezvousKeys(RendezvousClient::ServerKeys& outKeys, std::string* error = nullptr);

/// Absolute path of the config file that was loaded (or the first path probed
/// if none existed). Useful for diagnostics / UI. Empty before first load.
std::string getRendezvousConfigPath();

}
