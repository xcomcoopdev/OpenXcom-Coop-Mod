#pragma once

/*
 * Secure rendezvous client + server-list API for connectionUDP.
 *
 * Important security rule:
 *  - Room list returns public metadata only: roomId, name, player count, etc.
 *  - Room password and P2P sessionKey are never included in the room list.
 *  - sessionKey is returned only after an authenticated CREATE_ROOM/JOIN_ROOM and UDP_REGISTER.
 */

#include "connectionUDP.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace OpenXcom
{

class RendezvousClient
{
public:
    static const size_t kServerBoxPublicKeyBytes = crypto_box_PUBLICKEYBYTES;
    static const size_t kServerSignPublicKeyBytes = crypto_sign_PUBLICKEYBYTES;

    struct ServerKeys
    {
        std::array<unsigned char, kServerBoxPublicKeyBytes> serverBoxPublicKey{};
        std::array<unsigned char, kServerSignPublicKeyBytes> serverSignPublicKey{};
    };

    struct CommonConfig
    {
        std::string serverHost;
        uint16_t serverTcpPort = 0;
        uint16_t serverUdpPort = 0;
        ServerKeys keys;
        uint32_t timeoutMs = 30000;
        std::function<void(const std::string&)> log;

        // Optional cancellation hook used by UI disconnect/cancel while a
        // rendezvous wait is blocking on a worker thread.
        std::function<bool()> cancelRequested;
    };

    struct RoomInfo
    {
        std::string roomId;
        std::string name;
        std::string hostName;
        std::string region;
        uint32_t players = 0;
        uint32_t maxPlayers = 0;
        bool locked = false;
        bool passwordRequired = false;
        bool isCampaign = false;
        std::string gameVersion;
        std::string modHash;

        // True when this room was discovered on the local network via UDP broadcast.
        // LAN rooms still use the same rendezvous roomId/sessionKey flow, but the
        // final UDP peer endpoint is replaced with lanHost:lanPort to avoid
        // NAT hairpin / loopback problems on the same Wi-Fi/LAN.
        bool isLan = false;
        std::string lanHost;
        uint16_t lanPort = 0;
    };

    struct ListConfig : CommonConfig
    {
        std::string gameVersion;
        std::string modHash;
        bool compatibleOnly = false;
    };

    struct CreateRoomConfig : CommonConfig
    {
        std::string roomName;
        std::string playerName;
        std::string region;
        std::string password; // optional, never listed publicly
        bool listed = true;
        bool passwordRequired = false;
        bool isCampaign = false;
        std::string gameVersion;
        std::string modHash;
        uint32_t desiredPlayers = 2;

        // Must match the UDP local port later used by connectionUDP.
        // 0 means this client chooses a high ephemeral port explicitly.
        uint16_t localUdpPort = 0;

        // Called immediately after CREATE_ROOM_OK, before the host waits for
        // PEER_READY. This lets the UI/disconnect path close the listed room
        // even while createRoomAndWait(...) is still blocking.
        std::function<void(const std::string& roomId,
                           const std::string& hostToken,
                           uint64_t sessionId,
                           uint32_t playerId,
                           uint16_t localUdpPort)> onRoomCreated;
    };

    struct JoinRoomConfig : CommonConfig
    {
        std::string roomId;
        std::string playerName;
        std::string password; // empty for public rooms
        std::string gameVersion;
        std::string modHash;
        uint16_t localUdpPort = 0;
    };

    struct RoomControlConfig : CommonConfig
    {
        std::string roomId;
        std::string hostToken; // only the host receives this from CREATE_ROOM_OK
    };

    // Legacy manual room join/create. If room does not exist server creates it;
    // if it exists, server joins it. Useful for private invite-code testing.
    struct Config : CommonConfig
    {
        std::string roomId;
        std::string roomPassword;
        std::string playerName;
        std::string gameVersion;
        std::string modHash;
        uint16_t localUdpPort = 0;
        uint32_t desiredPlayers = 2;
    };

    struct Result
    {
        std::string roomId;
        std::string hostToken; // only set for the host that created a listed room
        std::string remoteHost;
        uint16_t remotePort = 0;
        uint16_t localPort = 0;
        uint64_t sessionId = 0;
        uint32_t localPlayerId = 0;
        uint32_t remotePlayerId = 0;
        std::array<unsigned char, connectionUDP::kSessionKeyBytes> sessionKey{};
        std::string peerPlayerName;
    };

    static bool listRooms(const ListConfig& cfg, std::vector<RoomInfo>& outRooms, std::string* error = nullptr);
    static bool createRoomAndWait(const CreateRoomConfig& cfg, Result& out, std::string* error = nullptr);
    static bool joinRoomAndWait(const JoinRoomConfig& cfg, Result& out, std::string* error = nullptr);

    // Host-only: closes/removes the listed room after the game lobby has locked.
    // This should be called only after connectionTCP::isCoopSessionLocked becomes true.
    static bool closeRoom(const RoomControlConfig& cfg, std::string* error = nullptr);

    // Compatibility wrapper for old manual JOIN flow.
    static bool perform(const Config& cfg, Result& out, std::string* error = nullptr);
};

} 
