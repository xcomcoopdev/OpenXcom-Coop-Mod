/*
 * Rendezvous/server-list -> connectionUDP glue.
 *
 * UI can call:
 *  - refreshServerListViaRendezvous(...)
 *  - hostListedViaRendezvous(...)
 *  - joinListedViaRendezvous(...)
 *  - startViaRendezvous(...) for manual/private room testing
 *
 * When rendezvous returns PEER_READY, this file starts the same connectionUDP
 * that writes incoming packets into g_rxQ. connectionTCP::updateCoopTask()
 * then parses those packets and calls connectionTCP::onTCPMessage(...), so UDP
 * and TCP use one packet execution path.
 */

#include "connection_rendezvous_glue.h"
#include "connection_udp_glue.h"
#include "connection_lan_discovery.h"
#include "rendezvous_config.h"
#include "../connectionTCP.h"

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace OpenXcom
{

extern int onConnect;
extern bool coopSession;
extern bool server_owner;
extern bool onTcpHost;

namespace
{
    std::mutex g_roomCloseMutex;
    bool g_roomCloseRequested = false;
    bool g_roomCloseDone = false;
    bool g_roomCloseMonitorRunning = false;
    std::atomic<bool> g_cancelRendezvous(false);

    std::string g_closeHost;
    uint16_t g_closeTcpPort = 0;
    uint16_t g_closeUdpPort = 0;
    std::string g_closeRoomId;
    std::string g_closeHostToken;
    RendezvousClient::ServerKeys g_closeKeys;

    struct LastHostConfig
    {
        bool valid = false;
        std::string rendezvousHost;
        uint16_t rendezvousTcpPort = 0;
        uint16_t rendezvousUdpPort = 0;
        std::string roomName;
        std::string roomPassword;
        std::string playerName;
        std::string region;
        std::string gameVersion;
        std::string modHash;
        bool listed = true;
        uint16_t localUdpPort = 0;
        RendezvousClient::ServerKeys keys;
    };

    std::mutex g_relistMutex;
    LastHostConfig g_lastHostConfig;
    bool g_relistInProgress = false;
}

static bool readKeyFile(const std::string& path, unsigned char* out, size_t n)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f)
        return false;
    f.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n));
    return f.gcount() == static_cast<std::streamsize>(n);
}

static bool loadPinnedServerKeys(const std::string& serverBoxPublicKeyPath,
                                 const std::string& serverSignPublicKeyPath,
                                 RendezvousClient::ServerKeys& keys)
{
    return readKeyFile(serverBoxPublicKeyPath, keys.serverBoxPublicKey.data(), keys.serverBoxPublicKey.size()) &&
           readKeyFile(serverSignPublicKeyPath, keys.serverSignPublicKey.data(), keys.serverSignPublicKey.size());
}

static void fillCommon(RendezvousClient::CommonConfig& cfg,
                       const std::string& rendezvousHost,
                       uint16_t rendezvousTcpPort,
                       uint16_t rendezvousUdpPort,
                       const RendezvousClient::ServerKeys& keys)
{
    cfg.serverHost = rendezvousHost;
    cfg.serverTcpPort = rendezvousTcpPort;
    cfg.serverUdpPort = rendezvousUdpPort;
    cfg.keys = keys;
    cfg.timeoutMs = 30000;
    cfg.cancelRequested = []() -> bool {
        return g_cancelRendezvous.load();
    };
    cfg.log = [](const std::string& s) {
        DebugLog(("Rendezvous: " + s + "\n").c_str());
    };
}

static bool startUdpFromRendezvousResult(const RendezvousClient::Result& rv,
                                         const std::string& playerName)
{
    const bool isHost = rv.localPlayerId == 1;
    return startUdpPeer(rv.remoteHost,
                        rv.remotePort,
                        rv.localPort,
                        rv.sessionId,
                        rv.sessionKey,
                        isHost,
                        playerName,
                        true);
}

static bool loadBuiltInKeysOrFail(RendezvousClient::ServerKeys& keys)
{
    std::string err;
    if (!loadBuiltInRendezvousKeys(keys, &err))
    {
        DebugLog(("Rendezvous built-in key load failed: " + err + "\n").c_str());
        onConnect = -3;
        return false;
    }
    return true;
}

static uint16_t normalizeHostLocalUdpPortForLanDiscovery(uint16_t localUdpPort)
{
    // UDP 3000 is reserved for LAN discovery. If the host explicitly tries to
    // use 3000 for the game transport, move the game UDP port to 3001 so the
    // discovery responder can keep listening on 3000.
    if (localUdpPort == kLanDiscoveryPort)
    {
        DebugLog("LAN discovery: UDP 3000 is reserved; using game UDP port 3001 instead\n");
        return static_cast<uint16_t>(3001);
    }
    return localUdpPort;
}

static void publishLanRoomForDiscovery(const std::string& roomId,
                                       const std::string& roomName,
                                       const std::string& hostName,
                                       const std::string& region,
                                       bool passwordRequired,
                                       const std::string& gameVersion,
                                       const std::string& modHash,
                                       uint32_t desiredPlayers,
                                       uint16_t localUdpPort)
{
    if (roomId.empty() || localUdpPort == 0)
        return;

    RendezvousClient::RoomInfo room;
    room.roomId = roomId;
    room.name = roomName;
    room.hostName = hostName.empty() ? std::string("Host") : hostName;
    room.region = region.empty() ? std::string("LAN") : region;
    room.players = 1;
    room.maxPlayers = desiredPlayers == 0 ? 2 : desiredPlayers;
    room.locked = false;
    room.passwordRequired = passwordRequired;
    room.gameVersion = gameVersion;
    room.modHash = modHash;
    room.isLan = true;
    room.lanPort = localUdpPort;

    startLanDiscoveryHost(room);
    DebugLog(("LAN discovery: advertising room " + roomId + " on UDP 3000, game UDP port " +
              std::to_string(localUdpPort) + "\n").c_str());
}



static bool closeListedRoomNow();
static void rememberHostRoomForClose(const std::string& rendezvousHost,
                                     uint16_t rendezvousTcpPort,
                                     uint16_t rendezvousUdpPort,
                                     const RendezvousClient::ServerKeys& keys,
                                     const RendezvousClient::Result& rv);


static void rememberLastHostConfig(const std::string& rendezvousHost,
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
                                   const RendezvousClient::ServerKeys& keys)
{
    std::lock_guard<std::mutex> lock(g_relistMutex);
    g_lastHostConfig.valid = true;
    g_lastHostConfig.rendezvousHost = rendezvousHost;
    g_lastHostConfig.rendezvousTcpPort = rendezvousTcpPort;
    g_lastHostConfig.rendezvousUdpPort = rendezvousUdpPort;
    g_lastHostConfig.roomName = roomName;
    g_lastHostConfig.roomPassword = roomPassword;
    g_lastHostConfig.playerName = playerName;
    g_lastHostConfig.region = region;
    g_lastHostConfig.gameVersion = gameVersion;
    g_lastHostConfig.modHash = modHash;
    g_lastHostConfig.listed = listed;
    g_lastHostConfig.localUdpPort = localUdpPort;
    g_lastHostConfig.keys = keys;
}

static void resetLobbyStateAfterRemoteDisconnect()
{
    // Remote player is gone. The old TCP disconnect branch will keep the host
    // alive, but the previous ready/locked state is no longer valid.
    connectionTCP::isCoopSessionLocked = false;
    connectionTCP::isPlayerReady = false;
    connectionTCP::isPlayersReady = false;
    connectionTCP::LobbyFileStatus = -1;
    connectionTCP::lobby_timer = -1;
}

static void reopenHostRoomAfterRemoteDisconnectAsync()
{
    LastHostConfig cfgCopy;
    {
        std::lock_guard<std::mutex> lock(g_relistMutex);
        if (!g_lastHostConfig.valid || g_relistInProgress)
            return;

        cfgCopy = g_lastHostConfig;
        g_relistInProgress = true;
    }

    std::thread([cfgCopy]() {
		// Give the old UDP socket time to close before opening a new one
		// with the same local UDP port during relist.
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        // The old full room/session cannot be listed again. Close it if it still
        // exists, then create a fresh listed room with a fresh rendezvous token.
        closeListedRoomNow();

        resetLobbyStateAfterRemoteDisconnect();
        coopSession = false;
        server_owner = true;
        onTcpHost = true;
        onConnect = 2; // host is alive and waiting for a replacement player

        g_cancelRendezvous.store(false);

        RendezvousClient::CreateRoomConfig cfg;
        fillCommon(cfg,
                   cfgCopy.rendezvousHost,
                   cfgCopy.rendezvousTcpPort,
                   cfgCopy.rendezvousUdpPort,
                   cfgCopy.keys);
        cfg.roomName = cfgCopy.roomName;
        cfg.playerName = cfgCopy.playerName;
        cfg.region = cfgCopy.region;
        cfg.password = cfgCopy.roomPassword;
        cfg.passwordRequired = !cfgCopy.roomPassword.empty();
        cfg.listed = cfgCopy.listed;
        cfg.gameVersion = cfgCopy.gameVersion;
        cfg.modHash = cfgCopy.modHash;
        cfg.desiredPlayers = 2;
        cfg.localUdpPort = cfgCopy.localUdpPort;
        cfg.timeoutMs = 0; // wait indefinitely for a new player
        cfg.onRoomCreated = [cfgCopy](const std::string& roomId,
                                      const std::string& hostToken,
                                      uint64_t,
                                      uint32_t,
                                      uint16_t actualLocalUdpPort) {
            RendezvousClient::Result partial;
            partial.roomId = roomId;
            partial.hostToken = hostToken;
            rememberHostRoomForClose(cfgCopy.rendezvousHost,
                                     cfgCopy.rendezvousTcpPort,
                                     cfgCopy.rendezvousUdpPort,
                                     cfgCopy.keys,
                                     partial);
            publishLanRoomForDiscovery(roomId, cfgCopy.roomName, cfgCopy.playerName, cfgCopy.region,
                                       !cfgCopy.roomPassword.empty(), cfgCopy.gameVersion,
                                       cfgCopy.modHash, 2, actualLocalUdpPort);
        };

        RendezvousClient::Result rv;
        std::string err;
        const bool ok = RendezvousClient::createRoomAndWait(cfg, rv, &err);
        if (!ok)
        {
            DebugLog(("Rendezvous relist after disconnect failed: " + err + "\n").c_str());
            if (err != "rendezvous cancelled")
                onConnect = -3;

            std::lock_guard<std::mutex> lock(g_relistMutex);
            g_relistInProgress = false;
            return;
        }

        DebugLog(("Rendezvous relist got new peer, room=" + rv.roomId + "\n").c_str());
        const bool udpOk = startUdpFromRendezvousResult(rv, cfgCopy.playerName);
        if (udpOk)
        {
            rememberHostRoomForClose(cfgCopy.rendezvousHost,
                                     cfgCopy.rendezvousTcpPort,
                                     cfgCopy.rendezvousUdpPort,
                                     cfgCopy.keys,
                                     rv);
        }

        std::lock_guard<std::mutex> lock(g_relistMutex);
        g_relistInProgress = false;
    }).detach();
}

static void setHostWaitingState()
{
    // Host button was pressed and the room is being listed / waiting.
    // Keep the old UI state as connected enough for LobbyMenu (onConnect = 1),
    // but do NOT mark coopSession true yet. coopSession becomes true only
    // after PEER_READY arrives and startUdpPeer(...) successfully starts the
    // real UDP peer connection.
    g_cancelRendezvous.store(false);
    coopSession = false;
    server_owner = true;
    onTcpHost = true;
    onConnect = 1;

    connectionTCP::isCoopSessionLocked = false;
    connectionTCP::isPlayerReady = false;
    connectionTCP::isPlayersReady = false;
    connectionTCP::LobbyFileStatus = -1;
    connectionTCP::lobby_timer = -1;
}

static bool closeListedRoomNow()
{
    RendezvousClient::RoomControlConfig cfg;
    {
        std::lock_guard<std::mutex> lock(g_roomCloseMutex);
        if (g_roomCloseDone || g_closeRoomId.empty() || g_closeHostToken.empty())
            return g_roomCloseDone;

        cfg.serverHost = g_closeHost;
        cfg.serverTcpPort = g_closeTcpPort;
        cfg.serverUdpPort = g_closeUdpPort;
        cfg.keys = g_closeKeys;
        cfg.timeoutMs = 5000;
        cfg.roomId = g_closeRoomId;
        cfg.hostToken = g_closeHostToken;
        cfg.log = [](const std::string& s) {
            DebugLog(("Rendezvous: " + s + "\n").c_str());
        };
    }

    std::string err;
    const bool ok = RendezvousClient::closeRoom(cfg, &err);
    {
        std::lock_guard<std::mutex> lock(g_roomCloseMutex);
        if (ok)
        {
            g_roomCloseDone = true;
            g_roomCloseRequested = false;
            stopLanDiscoveryHost();
            DebugLog("Rendezvous room closed after lobby lock\n");
        }
        else
        {
            DebugLog(("Rendezvous close room failed: " + err + "\n").c_str());
        }
    }
    return ok;
}

static void rememberHostRoomForClose(const std::string& rendezvousHost,
                                     uint16_t rendezvousTcpPort,
                                     uint16_t rendezvousUdpPort,
                                     const RendezvousClient::ServerKeys& keys,
                                     const RendezvousClient::Result& rv)
{
    if (rv.roomId.empty() || rv.hostToken.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(g_roomCloseMutex);
        g_closeHost = rendezvousHost;
        g_closeTcpPort = rendezvousTcpPort;
        g_closeUdpPort = rendezvousUdpPort;
        g_closeKeys = keys;
        g_closeRoomId = rv.roomId;
        g_closeHostToken = rv.hostToken;
        g_roomCloseRequested = true;
        g_roomCloseDone = false;

        if (g_roomCloseMonitorRunning)
            return;
        g_roomCloseMonitorRunning = true;
    }

    std::thread([]() {
        // The game/lobby code sets this static flag when every player has
        // clicked Ready. Only then do we close/remove the listed room from
        // the rendezvous server.
        while (true)
        {
            {
                std::lock_guard<std::mutex> lock(g_roomCloseMutex);
                if (!g_roomCloseRequested || g_roomCloseDone)
                {
                    g_roomCloseMonitorRunning = false;
                    return;
                }
            }

            if (connectionTCP::isCoopSessionLocked)
            {
                closeListedRoomNow();
                std::lock_guard<std::mutex> lock(g_roomCloseMutex);
                g_roomCloseMonitorRunning = false;
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }).detach();
}

void cancelRendezvousOperations()
{
    g_cancelRendezvous.store(true);
    stopLanDiscoveryHost();

    // If the host has already received CREATE_ROOM_OK while waiting for a
    // second player, close/remove the room from the rendezvous list.
    closeListedRoomNow();

    {
        std::lock_guard<std::mutex> lock(g_roomCloseMutex);
        g_roomCloseRequested = false;
        g_roomCloseMonitorRunning = false;
    }
}

void disconnectRendezvousUdp()
{
    // connectionTCP::disconnectTCP() decides the branch from the current
    // onConnect value. Do not overwrite full-close state here.
    const bool hostFullDisconnect = server_owner && onConnect == -1;
    const bool remotePeerLeftHostAlive = server_owner && onConnect == -2;

    cancelRendezvousOperations();
    stopUdpPeer();

    if (hostFullDisconnect)
    {
        server_owner = false;
        onTcpHost = false;
        coopSession = false;
        return;
    }

    if (remotePeerLeftHostAlive)
    {
        resetLobbyStateAfterRemoteDisconnect();
        coopSession = false;
        // connectionTCP::disconnectTCP() may turn -2 into 2. If it does not,
        // the relist worker will still expose waiting state before it creates
        // the fresh listed room.
        reopenHostRoomAfterRemoteDisconnectAsync();
    }
}

void handleUdpRemotePeerLost()
{
    // Called by connectionUDP glue when the UDP worker stops by itself. This
    // covers both a graceful F_CLOSE and a forced client shutdown detected by
    // UDP timeout. Do not call disconnectRendezvousUdp() here because this may
    // run from the UDP monitor thread; stopUdpPeer() would try to join itself.
    if (server_owner && onConnect != -1)
    {
        onConnect = -2;
        resetLobbyStateAfterRemoteDisconnect();
        coopSession = false;
        reopenHostRoomAfterRemoteDisconnectAsync();
        return;
    }

    if (!server_owner && onConnect != -1)
    {
        // Client lost the host/peer. This is a remote loss, not a user-requested
        // full host shutdown.
        onConnect = -2;
        coopSession = false;
    }
}


// Short UI-facing API. Uses rendezvous_config.cpp for host/ports/version/keys.
bool refreshServerListViaRendezvous(std::vector<RendezvousClient::RoomInfo>& outRooms)
{
    return refreshServerListViaRendezvous(std::string(), true, outRooms);
}

bool refreshServerListViaRendezvous(const std::string& modHash,
                                    bool compatibleOnly,
                                    std::vector<RendezvousClient::RoomInfo>& outRooms)
{
    const BuiltInRendezvousConfig cfg = getBuiltInRendezvousConfig();
    return refreshServerListViaRendezvous(cfg.host,
                                          cfg.tcpPort,
                                          cfg.udpPort,
                                          cfg.gameVersion,
                                          modHash,
                                          compatibleOnly,
                                          outRooms);
}

bool hostListedViaRendezvous(const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             uint16_t localUdpPort)
{
    return hostListedViaRendezvous(roomName,
                                   roomPassword,
                                   playerName,
                                   std::string(),
                                   std::string(),
                                   true,
                                   localUdpPort);
}

bool hostListedViaRendezvous(const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& region,
                             uint16_t localUdpPort)
{
    return hostListedViaRendezvous(roomName,
                                   roomPassword,
                                   playerName,
                                   region,
                                   std::string(),
                                   true,
                                   localUdpPort);
}

bool hostListedViaRendezvous(const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& modHash,
                             bool listed,
                             uint16_t localUdpPort)
{
    return hostListedViaRendezvous(roomName,
                                   roomPassword,
                                   playerName,
                                   std::string(),
                                   modHash,
                                   listed,
                                   localUdpPort);
}

bool hostListedViaRendezvous(const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& region,
                             const std::string& modHash,
                             bool listed,
                             uint16_t localUdpPort)
{
    const BuiltInRendezvousConfig cfg = getBuiltInRendezvousConfig();
    return hostListedViaRendezvous(cfg.host,
                                   cfg.tcpPort,
                                   cfg.udpPort,
                                   roomName,
                                   roomPassword,
                                   playerName,
                                   region,
                                   cfg.gameVersion,
                                   modHash,
                                   listed,
                                   localUdpPort);
}

bool joinListedViaRendezvous(const std::string& roomId,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             uint16_t localUdpPort)
{
    const BuiltInRendezvousConfig cfg = getBuiltInRendezvousConfig();
    return joinListedViaRendezvous(cfg.host,
                                   cfg.tcpPort,
                                   cfg.udpPort,
                                   roomId,
                                   roomPassword,
                                   playerName,
                                   localUdpPort);
}

bool joinLanRoomViaRendezvous(const std::string& roomId,
                              const std::string& lanHost,
                              uint16_t lanPort,
                              const std::string& roomPassword,
                              const std::string& playerName,
                              uint16_t localUdpPort)
{
    const BuiltInRendezvousConfig cfg = getBuiltInRendezvousConfig();
    return joinLanRoomViaRendezvous(cfg.host,
                                    cfg.tcpPort,
                                    cfg.udpPort,
                                    roomId,
                                    lanHost,
                                    lanPort,
                                    roomPassword,
                                    playerName,
                                    localUdpPort);
}

bool joinLanRoomByAddressViaRendezvous(const std::string& hostLanIp,
                                       const std::string& roomPassword,
                                       const std::string& playerName,
                                       uint16_t localUdpPort)
{
    const BuiltInRendezvousConfig cfg = getBuiltInRendezvousConfig();

    RendezvousClient::RoomInfo room;
    if (!findLanRoomByAddress(hostLanIp, room, cfg.gameVersion, std::string(), true, 750))
    {
        DebugLog(("LAN direct connect: no rendezvous LAN room found at " + hostLanIp + ":3000\n").c_str());
        onConnect = -3;
        return false;
    }

    DebugLog(("LAN direct connect: discovered room=" + room.roomId +
              " endpoint=" + room.lanHost + ":" + std::to_string(room.lanPort) + "\n").c_str());

    return joinLanRoomViaRendezvous(room.roomId,
                                    room.lanHost,
                                    room.lanPort,
                                    roomPassword,
                                    playerName,
                                    localUdpPort);
}

bool startViaRendezvous(const std::string& roomId,
                        const std::string& roomPassword,
                        const std::string& playerName,
                        uint16_t localUdpPort)
{
    const BuiltInRendezvousConfig cfg = getBuiltInRendezvousConfig();
    return startViaRendezvous(cfg.host,
                              cfg.tcpPort,
                              cfg.udpPort,
                              roomId,
                              roomPassword,
                              playerName,
                              localUdpPort);
}

static void callBoolCallback(RendezvousBoolCallback callback, bool ok)
{
    if (callback)
        callback(ok);
}

static void callListCallback(RendezvousListCallback callback,
                             bool ok,
                             std::vector<RendezvousClient::RoomInfo>&& rooms)
{
    if (callback)
        callback(ok, std::move(rooms));
}

void refreshServerListViaRendezvousAsync(RendezvousListCallback callback)
{
    refreshServerListViaRendezvousAsync(std::string(), true, std::move(callback));
}

void refreshServerListViaRendezvousAsync(const std::string& modHash,
                                         bool compatibleOnly,
                                         RendezvousListCallback callback)
{
    std::thread([modHash, compatibleOnly, callback]() mutable {
        std::vector<RendezvousClient::RoomInfo> rooms;
        const bool ok = refreshServerListViaRendezvous(modHash, compatibleOnly, rooms);
        callListCallback(std::move(callback), ok, std::move(rooms));
    }).detach();
}

void hostListedViaRendezvousAsync(const std::string& roomName,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback)
{
    hostListedViaRendezvousAsync(roomName,
                                 roomPassword,
                                 playerName,
                                 std::string(),
                                 localUdpPort,
                                 std::move(callback));
}

void hostListedViaRendezvousAsync(const std::string& roomName,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  const std::string& region,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback)
{
    hostListedViaRendezvousAsync(roomName,
                                 roomPassword,
                                 playerName,
                                 region,
                                 std::string(),
                                 true,
                                 localUdpPort,
                                 std::move(callback));
}

void hostListedViaRendezvousAsync(const std::string& roomName,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  const std::string& modHash,
                                  bool listed,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback)
{
    hostListedViaRendezvousAsync(roomName,
                                 roomPassword,
                                 playerName,
                                 std::string(),
                                 modHash,
                                 listed,
                                 localUdpPort,
                                 std::move(callback));
}

void hostListedViaRendezvousAsync(const std::string& roomName,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  const std::string& region,
                                  const std::string& modHash,
                                  bool listed,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback)
{
    std::thread([roomName, roomPassword, playerName, region, modHash, listed, localUdpPort, callback]() mutable {
        const bool ok = hostListedViaRendezvous(roomName,
                                                roomPassword,
                                                playerName,
                                                region,
                                                modHash,
                                                listed,
                                                localUdpPort);
        callBoolCallback(std::move(callback), ok);
    }).detach();
}

void joinListedViaRendezvousAsync(const std::string& roomId,
                                  const std::string& roomPassword,
                                  const std::string& playerName,
                                  uint16_t localUdpPort,
                                  RendezvousBoolCallback callback)
{
    std::thread([roomId, roomPassword, playerName, localUdpPort, callback]() mutable {
        // Intentional async delay before joining. This does not freeze the UI.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        const bool ok = joinListedViaRendezvous(roomId,
                                                roomPassword,
                                                playerName,
                                                localUdpPort);
        callBoolCallback(std::move(callback), ok);
    }).detach();
}

void joinLanRoomViaRendezvousAsync(const std::string& roomId,
                                   const std::string& lanHost,
                                   uint16_t lanPort,
                                   const std::string& roomPassword,
                                   const std::string& playerName,
                                   uint16_t localUdpPort,
                                   RendezvousBoolCallback callback)
{
    std::thread([roomId, lanHost, lanPort, roomPassword, playerName, localUdpPort, callback]() mutable {
        // Same async delay as internet join. This runs in the worker thread.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        const bool ok = joinLanRoomViaRendezvous(roomId,
                                                 lanHost,
                                                 lanPort,
                                                 roomPassword,
                                                 playerName,
                                                 localUdpPort);
        callBoolCallback(std::move(callback), ok);
    }).detach();
}

void joinLanRoomByAddressViaRendezvousAsync(const std::string& hostLanIp,
                                            const std::string& roomPassword,
                                            const std::string& playerName,
                                            uint16_t localUdpPort,
                                            RendezvousBoolCallback callback)
{
    std::thread([hostLanIp, roomPassword, playerName, localUdpPort, callback]() mutable {
        const bool ok = joinLanRoomByAddressViaRendezvous(hostLanIp,
                                                          roomPassword,
                                                          playerName,
                                                          localUdpPort);
        callBoolCallback(std::move(callback), ok);
    }).detach();
}

void startDirectLanHostAsync(const std::string& roomName,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             const std::string& region,
                             uint16_t localUdpPort,
                             RendezvousBoolCallback callback)
{
    // Important: this no longer creates a separate password-derived direct UDP
    // session. It creates one normal rendezvous room. LAN discovery then
    // advertises that same room locally, so internet and LAN rows share one
    // roomId/sessionId/sessionKey.
    hostListedViaRendezvousAsync(roomName, roomPassword, playerName, region, localUdpPort, std::move(callback));
}

void startDirectLanHostAsync(uint16_t localUdpPort,
                             const std::string& playerName,
                             const std::string& password,
                             RendezvousBoolCallback callback)
{
    startDirectLanHostAsync(std::string("LAN Game"), password, playerName, std::string("LAN"),
                            localUdpPort, std::move(callback));
}

void startViaRendezvousAsync(const std::string& roomId,
                             const std::string& roomPassword,
                             const std::string& playerName,
                             uint16_t localUdpPort,
                             RendezvousBoolCallback callback)
{
    std::thread([roomId, roomPassword, playerName, localUdpPort, callback]() mutable {
        const bool ok = startViaRendezvous(roomId,
                                           roomPassword,
                                           playerName,
                                           localUdpPort);
        callBoolCallback(std::move(callback), ok);
    }).detach();
}

// Simple API: uses public keys compiled into rendezvous_config.cpp.
bool refreshServerListViaRendezvous(const std::string& rendezvousHost,
                                    uint16_t rendezvousTcpPort,
                                    uint16_t rendezvousUdpPort,
                                    const std::string& gameVersion,
                                    const std::string& modHash,
                                    bool compatibleOnly,
                                    std::vector<RendezvousClient::RoomInfo>& outRooms)
{
    g_cancelRendezvous.store(false);
    outRooms.clear();

    RendezvousClient::ServerKeys keys;
    if (!loadBuiltInKeysOrFail(keys))
        return false;

    RendezvousClient::ListConfig cfg;
    fillCommon(cfg, rendezvousHost, rendezvousTcpPort, rendezvousUdpPort, keys);
    cfg.gameVersion = gameVersion;
    cfg.modHash = modHash;
    cfg.compatibleOnly = compatibleOnly;

    std::string err;
    if (!RendezvousClient::listRooms(cfg, outRooms, &err))
    {
        DebugLog(("Rendezvous list failed: " + err + "\n").c_str());
        return false;
    }

    // Merge LAN discovery results into the same server list. If the same roomId
    // is found locally, the existing internet row is marked as LAN so joining
    // uses lanHost:lanPort and avoids NAT hairpin / loopback problems.
    refreshLanServerList(outRooms, gameVersion, modHash, compatibleOnly);
    return true;
}

bool hostListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomName,
                             const std::string& optionalPassword,
                             const std::string& playerName,
                             const std::string& gameVersion,
                             const std::string& modHash,
                             bool listed,
                             uint16_t localUdpPort)
{
    return hostListedViaRendezvous(rendezvousHost,
                                   rendezvousTcpPort,
                                   rendezvousUdpPort,
                                   roomName,
                                   optionalPassword,
                                   playerName,
                                   std::string(),
                                   gameVersion,
                                   modHash,
                                   listed,
                                   localUdpPort);
}

bool hostListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomName,
                             const std::string& optionalPassword,
                             const std::string& playerName,
                             const std::string& region,
                             const std::string& gameVersion,
                             const std::string& modHash,
                             bool listed,
                             uint16_t localUdpPort)
{
    RendezvousClient::ServerKeys keys;
    if (!loadBuiltInKeysOrFail(keys))
        return false;

    const uint16_t hostGameUdpPort = normalizeHostLocalUdpPortForLanDiscovery(localUdpPort);

    rememberLastHostConfig(rendezvousHost, rendezvousTcpPort, rendezvousUdpPort,
                           roomName, optionalPassword, playerName, region, gameVersion,
                           modHash, listed, hostGameUdpPort, keys);

    setHostWaitingState();

    RendezvousClient::CreateRoomConfig cfg;
    fillCommon(cfg, rendezvousHost, rendezvousTcpPort, rendezvousUdpPort, keys);
    cfg.roomName = roomName;
    cfg.playerName = playerName;
    cfg.region = region;
    cfg.password = optionalPassword;
    cfg.passwordRequired = !optionalPassword.empty();
    cfg.listed = listed;
    cfg.gameVersion = gameVersion;
    cfg.modHash = modHash;
    cfg.desiredPlayers = 2;
    cfg.localUdpPort = hostGameUdpPort;
    cfg.timeoutMs = 0; // Host may wait indefinitely for the second player.
    cfg.onRoomCreated = [=, &keys](const std::string& roomId,
                                   const std::string& hostToken,
                                   uint64_t,
                                   uint32_t,
                                   uint16_t actualLocalUdpPort) {
        RendezvousClient::Result partial;
        partial.roomId = roomId;
        partial.hostToken = hostToken;
        rememberHostRoomForClose(rendezvousHost, rendezvousTcpPort, rendezvousUdpPort, keys, partial);
        publishLanRoomForDiscovery(roomId, roomName, playerName, region, !optionalPassword.empty(),
                                   gameVersion, modHash, 2, actualLocalUdpPort);
    };

    RendezvousClient::Result rv;
    std::string err;
    if (!RendezvousClient::createRoomAndWait(cfg, rv, &err))
    {
        DebugLog(("Rendezvous create failed: " + err + "\n").c_str());
        if (err == "rendezvous cancelled")
            onConnect = -2;
        else
            onConnect = -3;
        return false;
    }

    DebugLog(("Rendezvous created room " + rv.roomId + ", peer=" + rv.peerPlayerName + "\n").c_str());

    const bool udpOk = startUdpFromRendezvousResult(rv, playerName);
    if (udpOk)
    {
        rememberHostRoomForClose(rendezvousHost, rendezvousTcpPort, rendezvousUdpPort, keys, rv);
    }
    return udpOk;
}

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
                                  std::function<void(bool)> onComplete)
{
    hostListedViaRendezvousAsync(rendezvousHost,
                                 rendezvousTcpPort,
                                 rendezvousUdpPort,
                                 roomName,
                                 roomPassword,
                                 playerName,
                                 std::string(),
                                 gameVersion,
                                 modHash,
                                 listed,
                                 localUdpPort,
                                 std::move(onComplete));
}

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
                                  std::function<void(bool)> onComplete)
{
    // Make the old UI state visible immediately when the Host button is
    // pressed. The worker thread will do the blocking rendezvous wait.
    setHostWaitingState();

    std::thread([=]() {
        const bool ok = hostListedViaRendezvous(rendezvousHost,
                                                rendezvousTcpPort,
                                                rendezvousUdpPort,
                                                roomName,
                                                roomPassword,
                                                playerName,
                                                region,
                                                gameVersion,
                                                modHash,
                                                listed,
                                                localUdpPort);
        if (onComplete)
            onComplete(ok);
    }).detach();
}

bool joinListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomIdFromServerList,
                             const std::string& optionalPassword,
                             const std::string& playerName,
                             uint16_t localUdpPort)
{
    RendezvousClient::ServerKeys keys;
    if (!loadBuiltInKeysOrFail(keys))
        return false;

    g_cancelRendezvous.store(false);

    RendezvousClient::JoinRoomConfig cfg;
    fillCommon(cfg, rendezvousHost, rendezvousTcpPort, rendezvousUdpPort, keys);
    cfg.roomId = roomIdFromServerList;
    cfg.playerName = playerName;
    cfg.password = optionalPassword;
    cfg.localUdpPort = localUdpPort;

    RendezvousClient::Result rv;
    std::string err;
    if (!RendezvousClient::joinRoomAndWait(cfg, rv, &err))
    {
        DebugLog(("Rendezvous join failed: " + err + "\n").c_str());
        onConnect = -3;
        return false;
    }
    return startUdpFromRendezvousResult(rv, playerName);
}

bool joinLanRoomViaRendezvous(const std::string& rendezvousHost,
                              uint16_t rendezvousTcpPort,
                              uint16_t rendezvousUdpPort,
                              const std::string& roomIdFromServerList,
                              const std::string& lanHost,
                              uint16_t lanPort,
                              const std::string& optionalPassword,
                              const std::string& playerName,
                              uint16_t localUdpPort)
{
    RendezvousClient::ServerKeys keys;
    if (!loadBuiltInKeysOrFail(keys))
        return false;

    g_cancelRendezvous.store(false);

    RendezvousClient::JoinRoomConfig cfg;
    fillCommon(cfg, rendezvousHost, rendezvousTcpPort, rendezvousUdpPort, keys);
    cfg.roomId = roomIdFromServerList;
    cfg.playerName = playerName;
    cfg.password = optionalPassword;
    cfg.localUdpPort = localUdpPort;

    RendezvousClient::Result rv;
    std::string err;
    if (!RendezvousClient::joinRoomAndWait(cfg, rv, &err))
    {
        DebugLog(("Rendezvous LAN join failed: " + err + "\n").c_str());
        onConnect = -3;
        return false;
    }

    // NAT hairpin / loopback fix: rendezvous gave us the public endpoint,
    // but LAN discovery found the real local endpoint. Keep the same
    // rendezvous sessionId/sessionKey and only replace the UDP endpoint.
    rv.remoteHost = lanHost;
    rv.remotePort = lanPort;
    DebugLog(("Rendezvous LAN join using endpoint " + lanHost + ":" +
              std::to_string(lanPort) + "\n").c_str());

    return startUdpFromRendezvousResult(rv, playerName);
}

bool startViaRendezvous(const std::string& rendezvousHost,
                        uint16_t rendezvousTcpPort,
                        uint16_t rendezvousUdpPort,
                        const std::string& roomId,
                        const std::string& roomPassword,
                        const std::string& playerName,
                        uint16_t localUdpPort)
{
    RendezvousClient::ServerKeys keys;
    if (!loadBuiltInKeysOrFail(keys))
        return false;

    g_cancelRendezvous.store(false);

    RendezvousClient::Config cfg;
    fillCommon(cfg, rendezvousHost, rendezvousTcpPort, rendezvousUdpPort, keys);
    cfg.roomId = roomId;
    cfg.roomPassword = roomPassword;
    cfg.playerName = playerName;
    cfg.localUdpPort = localUdpPort;
    cfg.desiredPlayers = 2;
    cfg.timeoutMs = 0; // Private/manual first peer may wait indefinitely.

    RendezvousClient::Result rv;
    std::string err;
    if (!RendezvousClient::perform(cfg, rv, &err))
    {
        DebugLog(("Rendezvous failed: " + err + "\n").c_str());
        onConnect = -3;
        return false;
    }
    return startUdpFromRendezvousResult(rv, playerName);
}


bool closeListedRoomViaRendezvous()
{
    // Manual safety hook. Normally hostListedViaRendezvous(...) starts a
    // background monitor which calls this automatically after
    // connectionTCP::isCoopSessionLocked becomes true.
    if (!connectionTCP::isCoopSessionLocked)
        return false;

    return closeListedRoomNow();
}

bool refreshServerListViaRendezvous(const std::string& rendezvousHost,
                                    uint16_t rendezvousTcpPort,
                                    uint16_t rendezvousUdpPort,
                                    const std::string& gameVersion,
                                    const std::string& modHash,
                                    bool compatibleOnly,
                                    const std::string& serverBoxPublicKeyPath,
                                    const std::string& serverSignPublicKeyPath,
                                    std::vector<RendezvousClient::RoomInfo>& outRooms)
{
    g_cancelRendezvous.store(false);
    outRooms.clear();

    RendezvousClient::ServerKeys keys;
    if (!loadPinnedServerKeys(serverBoxPublicKeyPath, serverSignPublicKeyPath, keys))
    {
        onConnect = -3;
        return false;
    }

    RendezvousClient::ListConfig cfg;
    fillCommon(cfg, rendezvousHost, rendezvousTcpPort, rendezvousUdpPort, keys);
    cfg.gameVersion = gameVersion;
    cfg.modHash = modHash;
    cfg.compatibleOnly = compatibleOnly;

    std::string err;
    if (!RendezvousClient::listRooms(cfg, outRooms, &err))
    {
        DebugLog(("Rendezvous list failed: " + err + "\n").c_str());
        return false;
    }
    return true;
}


bool hostListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomName,
                             const std::string& optionalPassword,
                             const std::string& playerName,
                             const std::string& gameVersion,
                             const std::string& modHash,
                             bool listed,
                             uint16_t localUdpPort,
                             const std::string& serverBoxPublicKeyPath,
                             const std::string& serverSignPublicKeyPath)
{
    return hostListedViaRendezvous(rendezvousHost,
                                   rendezvousTcpPort,
                                   rendezvousUdpPort,
                                   roomName,
                                   optionalPassword,
                                   playerName,
                                   std::string(),
                                   gameVersion,
                                   modHash,
                                   listed,
                                   localUdpPort,
                                   serverBoxPublicKeyPath,
                                   serverSignPublicKeyPath);
}

bool hostListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomName,
                             const std::string& optionalPassword,
                             const std::string& playerName,
                             const std::string& region,
                             const std::string& gameVersion,
                             const std::string& modHash,
                             bool listed,
                             uint16_t localUdpPort,
                             const std::string& serverBoxPublicKeyPath,
                             const std::string& serverSignPublicKeyPath)
{
    RendezvousClient::ServerKeys keys;
    if (!loadPinnedServerKeys(serverBoxPublicKeyPath, serverSignPublicKeyPath, keys))
    {
        onConnect = -3;
        return false;
    }

    const uint16_t hostGameUdpPort = normalizeHostLocalUdpPortForLanDiscovery(localUdpPort);

    rememberLastHostConfig(rendezvousHost, rendezvousTcpPort, rendezvousUdpPort,
                           roomName, optionalPassword, playerName, region, gameVersion,
                           modHash, listed, hostGameUdpPort, keys);

    setHostWaitingState();

    RendezvousClient::CreateRoomConfig cfg;
    fillCommon(cfg, rendezvousHost, rendezvousTcpPort, rendezvousUdpPort, keys);
    cfg.roomName = roomName;
    cfg.playerName = playerName;
    cfg.region = region;
    cfg.password = optionalPassword;
    cfg.passwordRequired = !optionalPassword.empty();
    cfg.listed = listed;
    cfg.gameVersion = gameVersion;
    cfg.modHash = modHash;
    cfg.desiredPlayers = 2;
    cfg.localUdpPort = hostGameUdpPort;
    cfg.timeoutMs = 0; // Host may wait indefinitely for the second player.
    cfg.onRoomCreated = [=, &keys](const std::string& roomId,
                                   const std::string& hostToken,
                                   uint64_t,
                                   uint32_t,
                                   uint16_t actualLocalUdpPort) {
        RendezvousClient::Result partial;
        partial.roomId = roomId;
        partial.hostToken = hostToken;
        rememberHostRoomForClose(rendezvousHost, rendezvousTcpPort, rendezvousUdpPort, keys, partial);
        publishLanRoomForDiscovery(roomId, roomName, playerName, region, !optionalPassword.empty(),
                                   gameVersion, modHash, 2, actualLocalUdpPort);
    };

    RendezvousClient::Result rv;
    std::string err;
    if (!RendezvousClient::createRoomAndWait(cfg, rv, &err))
    {
        DebugLog(("Rendezvous create failed: " + err + "\n").c_str());
        if (err == "rendezvous cancelled")
            onConnect = -2;
        else
            onConnect = -3;
        return false;
    }

    DebugLog(("Rendezvous created room " + rv.roomId + ", peer=" + rv.peerPlayerName + "\n").c_str());

    const bool udpOk = startUdpFromRendezvousResult(rv, playerName);
    if (udpOk)
    {
        rememberHostRoomForClose(rendezvousHost, rendezvousTcpPort, rendezvousUdpPort, keys, rv);
    }
    return udpOk;
}

bool joinListedViaRendezvous(const std::string& rendezvousHost,
                             uint16_t rendezvousTcpPort,
                             uint16_t rendezvousUdpPort,
                             const std::string& roomIdFromServerList,
                             const std::string& optionalPassword,
                             const std::string& playerName,
                             uint16_t localUdpPort,
                             const std::string& serverBoxPublicKeyPath,
                             const std::string& serverSignPublicKeyPath)
{
    RendezvousClient::ServerKeys keys;
    if (!loadPinnedServerKeys(serverBoxPublicKeyPath, serverSignPublicKeyPath, keys))
    {
        onConnect = -3;
        return false;
    }

    RendezvousClient::JoinRoomConfig cfg;
    fillCommon(cfg, rendezvousHost, rendezvousTcpPort, rendezvousUdpPort, keys);
    cfg.roomId = roomIdFromServerList;
    cfg.playerName = playerName;
    cfg.password = optionalPassword;
    cfg.localUdpPort = localUdpPort;

    RendezvousClient::Result rv;
    std::string err;
    if (!RendezvousClient::joinRoomAndWait(cfg, rv, &err))
    {
        DebugLog(("Rendezvous join failed: " + err + "\n").c_str());
        onConnect = -3;
        return false;
    }
    return startUdpFromRendezvousResult(rv, playerName);
}

// Manual invite-code API. If room does not exist, server creates it; otherwise
// server joins it. Useful for private-room testing.
bool startViaRendezvous(const std::string& rendezvousHost,
                        uint16_t rendezvousTcpPort,
                        uint16_t rendezvousUdpPort,
                        const std::string& roomId,
                        const std::string& roomPassword,
                        const std::string& playerName,
                        uint16_t localUdpPort,
                        const std::string& serverBoxPublicKeyPath,
                        const std::string& serverSignPublicKeyPath)
{
    RendezvousClient::ServerKeys keys;
    if (!loadPinnedServerKeys(serverBoxPublicKeyPath, serverSignPublicKeyPath, keys))
    {
        onConnect = -3;
        return false;
    }

    RendezvousClient::Config cfg;
    fillCommon(cfg, rendezvousHost, rendezvousTcpPort, rendezvousUdpPort, keys);
    cfg.roomId = roomId;
    cfg.roomPassword = roomPassword;
    cfg.playerName = playerName;
    cfg.localUdpPort = localUdpPort;
    cfg.desiredPlayers = 2;
    cfg.timeoutMs = 0; // Private/manual first peer may wait indefinitely.

    RendezvousClient::Result rv;
    std::string err;
    if (!RendezvousClient::perform(cfg, rv, &err))
    {
        DebugLog(("Rendezvous failed: " + err + "\n").c_str());
        onConnect = -3;
        return false;
    }
    return startUdpFromRendezvousResult(rv, playerName);
}

// Compatibility wrappers for names used in the previous drafts.
bool refreshCoopServerListViaRendezvous(const std::string& rendezvousHost,
                                        uint16_t rendezvousTcpPort,
                                        uint16_t rendezvousUdpPort,
                                        const std::string& gameVersion,
                                        const std::string& modHash,
                                        bool compatibleOnly,
                                        const std::string& serverBoxPublicKeyPath,
                                        const std::string& serverSignPublicKeyPath,
                                        std::vector<RendezvousClient::RoomInfo>& outRooms)
{
    return refreshServerListViaRendezvous(rendezvousHost, rendezvousTcpPort, rendezvousUdpPort,
                                          gameVersion, modHash, compatibleOnly,
                                          serverBoxPublicKeyPath, serverSignPublicKeyPath,
                                          outRooms);
}

bool hostListedCoopViaRendezvous(const std::string& rendezvousHost,
                                 uint16_t rendezvousTcpPort,
                                 uint16_t rendezvousUdpPort,
                                 const std::string& roomName,
                                 const std::string& optionalPassword,
                                 const std::string& playerName,
                                 const std::string& gameVersion,
                                 const std::string& modHash,
                                 bool listed,
                                 uint16_t localUdpPort,
                                 const std::string& serverBoxPublicKeyPath,
                                 const std::string& serverSignPublicKeyPath)
{
    return hostListedViaRendezvous(rendezvousHost, rendezvousTcpPort, rendezvousUdpPort,
                                   roomName, optionalPassword, playerName, std::string(), gameVersion,
                                   modHash, listed, localUdpPort,
                                   serverBoxPublicKeyPath, serverSignPublicKeyPath);
}

bool joinListedCoopViaRendezvous(const std::string& rendezvousHost,
                                 uint16_t rendezvousTcpPort,
                                 uint16_t rendezvousUdpPort,
                                 const std::string& roomIdFromServerList,
                                 const std::string& optionalPassword,
                                 const std::string& playerName,
                                 uint16_t localUdpPort,
                                 const std::string& serverBoxPublicKeyPath,
                                 const std::string& serverSignPublicKeyPath)
{
    return joinListedViaRendezvous(rendezvousHost, rendezvousTcpPort, rendezvousUdpPort,
                                   roomIdFromServerList, optionalPassword, playerName,
                                   localUdpPort, serverBoxPublicKeyPath, serverSignPublicKeyPath);
}

bool startCoopViaRendezvous(const std::string& rendezvousHost,
                            uint16_t rendezvousTcpPort,
                            uint16_t rendezvousUdpPort,
                            const std::string& roomId,
                            const std::string& roomPassword,
                            const std::string& playerName,
                            uint16_t localUdpPort,
                            const std::string& serverBoxPublicKeyPath,
                            const std::string& serverSignPublicKeyPath)
{
    return startViaRendezvous(rendezvousHost, rendezvousTcpPort, rendezvousUdpPort,
                              roomId, roomPassword, playerName, localUdpPort,
                              serverBoxPublicKeyPath, serverSignPublicKeyPath);
}

} 
