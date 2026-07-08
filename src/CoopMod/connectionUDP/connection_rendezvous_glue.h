#pragma once

#include "rendezvous_client.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace OpenXcom
{

using RendezvousBoolCallback = std::function<void(bool ok)>;
using RendezvousListCallback = std::function<void(bool ok, std::vector<RendezvousClient::RoomInfo> rooms)>;

// Result of a single server health probe (see probeRendezvousServer).
struct RendezvousProbeResult
{
    size_t index = 0;                             // configured-server index
    bool online = false;                          // answered a signed ROOM_LIST
    std::vector<RendezvousClient::RoomInfo> rooms; // populated when online
};
using RendezvousProbeCallback = std::function<void(RendezvousProbeResult)>;

// Punch-free health check of one configured server (by index). Blocking.
// Sends LIST_ROOMS over TCP with a short timeout; success proves the rendezvous
// service is up and its sign key validates. Does not touch the active server or
// the live coop flow. When outRooms is non-null it receives the room list.
bool probeRendezvousServer(size_t serverIndex,
                           uint32_t timeoutMs,
                           std::vector<RendezvousClient::RoomInfo>* outRooms,
                           std::string* error = nullptr);

// Probes every configured server in parallel (one detached thread each). The
// callback fires once per server as its probe completes, on a worker thread —
// do not modify UI directly from it.
void probeAllRendezvousServersAsync(uint32_t timeoutMs, RendezvousProbeCallback callback);

// True when the current multiplayer flow uses rendezvous/server-list/UDP.
// TCP-only disconnects use this to avoid closing stale rendezvous state.
bool isRendezvousConnectionActive();

// -----------------------------------------------------------------------------
// Short UI-facing API.
// Uses rendezvous_config.cpp for host, ports, game version and public keys.
// -----------------------------------------------------------------------------
bool refreshServerListViaRendezvous(std::vector<RendezvousClient::RoomInfo>& outRooms);
bool refreshServerListViaRendezvous(const std::string& modHash,
                                    bool compatibleOnly,
                                    std::vector<RendezvousClient::RoomInfo>& outRooms);

bool hostListedViaRendezvous(const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             uint16_t localUdpPort);
bool hostListedViaRendezvous(const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& region,
                             uint16_t localUdpPort);
bool hostListedViaRendezvous(const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& modHash,
                             bool listed,
                             uint16_t localUdpPort);
bool hostListedViaRendezvous(const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& modHash,
                             bool listed,
                             bool isCampaign,
                             uint16_t localUdpPort);
bool hostListedViaRendezvous(const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& region,
                             const std::string& modHash,
                             bool listed,
                             uint16_t localUdpPort);
bool hostListedViaRendezvous(const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& region,
                             const std::string& modHash,
                             bool listed,
                             bool isCampaign,
                             uint16_t localUdpPort);

bool joinListedViaRendezvous(const std::string& roomId,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             uint16_t localUdpPort);
bool joinListedViaRendezvous(const std::string& roomId,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& modHash,
                             uint16_t localUdpPort);

// LAN discovery / NAT hairpin path. This still joins the same rendezvous room
// to get the sessionId/sessionKey, but replaces the public UDP endpoint with
// lanHost:lanPort before starting connectionUDP.
bool joinLanRoomViaRendezvous(const std::string& roomId,
                              const std::string& lanHost,
                              uint16_t lanPort,
                              const std::string& roomPassword,
                              const std::string& playerName,
                              uint16_t localUdpPort);
bool joinLanRoomViaRendezvous(const std::string& roomId,
                              const std::string& lanHost,
                              uint16_t lanPort,
                              const std::string& roomPassword,
                              const std::string& playerName,
                              const std::string& modHash,
                              uint16_t localUdpPort);

// Direct Connect helper for LAN hosts that were created with
// hostListedViaRendezvousAsync(...). The user only provides the host LAN IP;
// this function queries that host on kLanDiscoveryPort to get roomId/lanPort, then joins
// the same rendezvous room through the LAN endpoint.
bool joinLanRoomByAddressViaRendezvous(const std::string& hostLanIp,
                                       const std::string& roomPassword,
                                       const std::string& playerName,
                                       uint16_t localUdpPort);
bool joinLanRoomByAddressViaRendezvous(const std::string& hostLanIp,
                                       const std::string& roomPassword,
                                       const std::string& playerName,
                                       const std::string& modHash,
                                       uint16_t localUdpPort);

bool startViaRendezvous(const std::string& roomId,
                        const std::string& roomPassword,
                        const std::string& playerName,
                        uint16_t localUdpPort);

// -----------------------------------------------------------------------------
// Async UI-facing API.
// These return immediately. Callback runs on the worker thread.
// Do not directly modify UI from the callback.
// -----------------------------------------------------------------------------
void refreshServerListViaRendezvousAsync(RendezvousListCallback callback);
void refreshServerListViaRendezvousAsync(const std::string& modHash,
                                         bool compatibleOnly,
                                         RendezvousListCallback callback);

void hostListedViaRendezvousAsync(const std::string& roomName,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback = RendezvousBoolCallback());
void hostListedViaRendezvousAsync(const std::string& roomName,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  const std::string& region,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback = RendezvousBoolCallback());
void hostListedViaRendezvousAsync(const std::string& roomName,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  const std::string& modHash,
                                  bool listed,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback = RendezvousBoolCallback());
void hostListedViaRendezvousAsync(const std::string& roomName,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  const std::string& modHash,
                                  bool listed,
                                  bool isCampaign,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback = RendezvousBoolCallback());
void hostListedViaRendezvousAsync(const std::string& roomName,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  const std::string& region,
                                  const std::string& modHash,
                                  bool listed,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback = RendezvousBoolCallback());
void hostListedViaRendezvousAsync(const std::string& roomName,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  const std::string& region,
                                  const std::string& modHash,
                                  bool listed,
                                  bool isCampaign,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback = RendezvousBoolCallback());

void joinListedViaRendezvousAsync(const std::string& roomId,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback = RendezvousBoolCallback());
void joinListedViaRendezvousAsync(const std::string& roomId,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  const std::string& modHash,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback = RendezvousBoolCallback());

void joinLanRoomViaRendezvousAsync(const std::string& roomId,
                                   const std::string& lanHost,
                                   uint16_t lanPort,
                                   const std::string& roomPassword,
                                   const std::string& playerName,
                                   uint16_t localUdpPort,
                                   RendezvousBoolCallback callback = RendezvousBoolCallback());
void joinLanRoomViaRendezvousAsync(const std::string& roomId,
                                   const std::string& lanHost,
                                   uint16_t lanPort,
                                   const std::string& roomPassword,
                                   const std::string& playerName,
                                   const std::string& modHash,
                                   uint16_t localUdpPort,
                                   RendezvousBoolCallback callback = RendezvousBoolCallback());

void joinLanRoomByAddressViaRendezvousAsync(const std::string& hostLanIp,
                                            const std::string& roomPassword,
                                            const std::string& playerName,
                                            uint16_t localUdpPort,
                                            RendezvousBoolCallback callback = RendezvousBoolCallback());
void joinLanRoomByAddressViaRendezvousAsync(const std::string& hostLanIp,
                                            const std::string& roomPassword,
                                            const std::string& playerName,
                                            const std::string& modHash,
                                            uint16_t localUdpPort,
                                            RendezvousBoolCallback callback = RendezvousBoolCallback());

void startDirectLanHostAsync(uint16_t localUdpPort,
                             const std::string& playerName,
                             const std::string& password,
                             RendezvousBoolCallback callback = RendezvousBoolCallback());

void startViaRendezvousAsync(const std::string& roomId,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             uint16_t localUdpPort,
                             RendezvousBoolCallback callback = RendezvousBoolCallback());

// Manual hook used by connectionTCP::disconnectTCP().
void cancelRendezvousOperations();
void disconnectRendezvousUdp();
void handleUdpRemotePeerLost();
bool closeListedRoomViaRendezvous();

// -----------------------------------------------------------------------------
// Explicit API kept for tests/debug/custom rendezvous servers.
// These use public keys compiled into rendezvous_config.cpp.
// -----------------------------------------------------------------------------
bool refreshServerListViaRendezvous(const std::string& rendezvousHost,
                                    uint16_t rendezvousTcpPort,
                                    uint16_t rendezvousUdpPort,
                                    const std::string& gameVersion,
                                    const std::string& modHash,
                                    bool compatibleOnly,
                                    std::vector<RendezvousClient::RoomInfo>& outRooms);

bool hostListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& gameVersion,
                             const std::string& modHash,
                             bool listed,
                             uint16_t localUdpPort);
bool hostListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& gameVersion,
                             const std::string& modHash,
                             bool listed,
                             bool isCampaign,
                             uint16_t localUdpPort);

bool hostListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& region,
                             const std::string& gameVersion,
                             const std::string& modHash,
                             bool listed,
                             uint16_t localUdpPort);
bool hostListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& region,
                             const std::string& gameVersion,
                             const std::string& modHash,
                             bool listed,
                             bool isCampaign,
                             uint16_t localUdpPort);

void hostListedViaRendezvousAsync(const std::string& rendezvousHost,
                                  uint16_t rendezvousTcpPort,
                                  uint16_t rendezvousUdpPort,
                                  const std::string& roomName,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  const std::string& gameVersion,
                                  const std::string& modHash,
                                  bool listed,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback = RendezvousBoolCallback());

void hostListedViaRendezvousAsync(const std::string& rendezvousHost,
                                  uint16_t rendezvousTcpPort,
                                  uint16_t rendezvousUdpPort,
                                  const std::string& roomName,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  const std::string& region,
                                  const std::string& gameVersion,
                                  const std::string& modHash,
                                  bool listed,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback = RendezvousBoolCallback());

bool joinListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomId,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             uint16_t localUdpPort);
bool joinListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomId,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& gameVersion,
                             const std::string& modHash,
                             uint16_t localUdpPort);

bool joinLanRoomViaRendezvous(const std::string& rendezvousHost,
                              uint16_t rendezvousTcpPort,
                              uint16_t rendezvousUdpPort,
                              const std::string& roomId,
                              const std::string& lanHost,
                              uint16_t lanPort,
                              const std::string& roomPassword,
                              const std::string& playerName,
                              uint16_t localUdpPort);
bool joinLanRoomViaRendezvous(const std::string& rendezvousHost,
                              uint16_t rendezvousTcpPort,
                              uint16_t rendezvousUdpPort,
                              const std::string& roomId,
                              const std::string& lanHost,
                              uint16_t lanPort,
                              const std::string& roomPassword,
                              const std::string& playerName,
                              const std::string& gameVersion,
                              const std::string& modHash,
                              uint16_t localUdpPort);

void joinListedViaRendezvousAsync(const std::string& rendezvousHost,
                                  uint16_t rendezvousTcpPort,
                                  uint16_t rendezvousUdpPort,
                                  const std::string& roomId,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback = RendezvousBoolCallback());

bool startViaRendezvous(const std::string& rendezvousHost,
                        uint16_t rendezvousTcpPort,
                        uint16_t rendezvousUdpPort,
                        const std::string& roomId,
                        const std::string& roomPassword,
                        const std::string& playerName,
                        uint16_t localUdpPort);

// Explicit key-file API kept only for older tests/debug.
bool refreshServerListViaRendezvous(const std::string& rendezvousHost,
                                    uint16_t rendezvousTcpPort,
                                    uint16_t rendezvousUdpPort,
                                    const std::string& gameVersion,
                                    const std::string& modHash,
                                    bool compatibleOnly,
                                    const std::string& serverBoxPublicKeyPath,
                                    const std::string& serverSignPublicKeyPath,
                                    std::vector<RendezvousClient::RoomInfo>& outRooms);

bool hostListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& gameVersion,
                             const std::string& modHash,
                             bool listed,
                             uint16_t localUdpPort,
                             const std::string& serverBoxPublicKeyPath,
                             const std::string& serverSignPublicKeyPath);

bool hostListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& region,
                             const std::string& gameVersion,
                             const std::string& modHash,
                             bool listed,
                             uint16_t localUdpPort,
                             const std::string& serverBoxPublicKeyPath,
                             const std::string& serverSignPublicKeyPath);

bool joinListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomId,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             uint16_t localUdpPort,
                             const std::string& serverBoxPublicKeyPath,
                             const std::string& serverSignPublicKeyPath);

bool startViaRendezvous(const std::string& rendezvousHost,
                        uint16_t rendezvousTcpPort,
                        uint16_t rendezvousUdpPort,
                        const std::string& roomId,
                        const std::string& roomPassword,
                        const std::string& playerName,
                        uint16_t localUdpPort,
                        const std::string& serverBoxPublicKeyPath,
                        const std::string& serverSignPublicKeyPath);

// Backwards-compatible names from earlier drafts.
bool refreshCoopServerListViaRendezvous(const std::string& rendezvousHost,
                                        uint16_t rendezvousTcpPort,
                                        uint16_t rendezvousUdpPort,
                                        const std::string& gameVersion,
                                        const std::string& modHash,
                                        bool compatibleOnly,
                                        const std::string& serverBoxPublicKeyPath,
                                        const std::string& serverSignPublicKeyPath,
                                        std::vector<RendezvousClient::RoomInfo>& outRooms);

bool hostListedCoopViaRendezvous(const std::string& rendezvousHost,
                                 uint16_t rendezvousTcpPort,
                                 uint16_t rendezvousUdpPort,
                                 const std::string& roomName,
                                 const std::string& roomPassword,
                                 const std::string& playerName,
                                 const std::string& gameVersion,
                                 const std::string& modHash,
                                 bool listed,
                                 uint16_t localUdpPort,
                                 const std::string& serverBoxPublicKeyPath,
                                 const std::string& serverSignPublicKeyPath);

bool joinListedCoopViaRendezvous(const std::string& rendezvousHost,
                                 uint16_t rendezvousTcpPort,
                                 uint16_t rendezvousUdpPort,
                                 const std::string& roomId,
                                 const std::string& roomPassword,
                                 const std::string& playerName,
                                 uint16_t localUdpPort,
                                 const std::string& serverBoxPublicKeyPath,
                                 const std::string& serverSignPublicKeyPath);

bool startCoopViaRendezvous(const std::string& rendezvousHost,
                            uint16_t rendezvousTcpPort,
                            uint16_t rendezvousUdpPort,
                            const std::string& roomId,
                            const std::string& roomPassword,
                            const std::string& playerName,
                            uint16_t localUdpPort,
                            const std::string& serverBoxPublicKeyPath,
                            const std::string& serverSignPublicKeyPath);

} 
