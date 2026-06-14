#include "connection_lan_discovery.h"
#include "../connectionTCP.h"

#include <SDL_net.h>
#include <json/json.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace OpenXcom
{
namespace
{
    static const uint32_t kLanProtocolVersion = 2;
    static const char* kDiscoverType = "OPENXCOM_LAN_DISCOVER";
    static const char* kRoomType = "OPENXCOM_LAN_ROOM";

    std::mutex g_lanMutex;
    RendezvousClient::RoomInfo g_lanRoom;
    bool g_lanRoomValid = false;
    std::atomic<bool> g_lanStop(false);
    std::thread g_lanThread;

    std::string compactJson(const Json::Value& v)
    {
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        return Json::writeString(wb, v);
    }

    bool parseJson(const std::string& s, Json::Value& v)
    {
        Json::CharReaderBuilder rb;
        std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
        std::string errs;
        return reader->parse(s.data(), s.data() + s.size(), &v, &errs);
    }

    std::string ipToString(const IPaddress& a)
    {
        const Uint32 host = SDL_SwapBE32(a.host);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                      static_cast<unsigned>((host >> 24) & 0xff),
                      static_cast<unsigned>((host >> 16) & 0xff),
                      static_cast<unsigned>((host >> 8) & 0xff),
                      static_cast<unsigned>(host & 0xff));
        return std::string(buf);
    }

    void mergeRoom(std::vector<RendezvousClient::RoomInfo>& ioRooms,
                   const RendezvousClient::RoomInfo& lanRoom)
    {
        for (auto& existing : ioRooms)
        {
            if (!existing.roomId.empty() && existing.roomId == lanRoom.roomId)
            {
                // Same room exists from the internet server list. Prefer LAN
                // endpoint locally, so joining this row avoids NAT hairpin.
                existing.isLan = true;
                existing.lanHost = lanRoom.lanHost;
                existing.lanPort = lanRoom.lanPort;
                if (!lanRoom.region.empty()) existing.region = lanRoom.region;
                existing.isCampaign = lanRoom.isCampaign;
                return;
            }
        }
        ioRooms.push_back(lanRoom);
    }


    bool roomFromLanResponse(const Json::Value& msg,
                             const IPaddress& sourceAddress,
                             const std::string& gameVersion,
                             const std::string& modHash,
                             bool compatibleOnly,
                             RendezvousClient::RoomInfo& room)
    {
        if (msg.get("type", "").asString() != kRoomType)
            return false;
        if (msg.get("version", 0).asUInt() != kLanProtocolVersion)
            return false;

        room = RendezvousClient::RoomInfo();
        room.isLan = true;
        room.roomId = msg.get("room_id", "").asString();
        room.name = msg.get("room_name", "LAN Game").asString();
        room.hostName = msg.get("host_name", "Host").asString();
        room.region = msg.get("region", "LAN").asString();
        room.players = msg.get("players", 1).asUInt();
        room.maxPlayers = msg.get("max_players", 2).asUInt();
        room.passwordRequired = msg.get("password_required", false).asBool();
        room.isCampaign = msg.get("is_campaign", false).asBool();
        room.gameVersion = msg.get("game_version", "").asString();
        room.modHash = msg.get("mod_hash", "").asString();
        room.lanHost = ipToString(sourceAddress);
        room.lanPort = static_cast<uint16_t>(msg.get("udp_port", 0).asUInt());

        if (room.roomId.empty() || room.lanHost.empty() || room.lanPort == 0)
            return false;

        if (compatibleOnly)
        {
            if (!gameVersion.empty() && !room.gameVersion.empty() && room.gameVersion != gameVersion)
                return false;
            if (!modHash.empty() && !room.modHash.empty() && room.modHash != modHash)
                return false;
        }

        return true;
    }

    void lanResponderThread()
    {
        if (SDLNet_Init() == -1)
        {
            DebugLog("LAN discovery: SDLNet_Init failed; LAN list will be disabled\n");
            return;
        }

        UDPsocket sock = SDLNet_UDP_Open(kLanDiscoveryPort);
        if (!sock)
        {
            DebugLog(("LAN discovery: could not open UDP port " + std::to_string(kLanDiscoveryPort) +
                      "; LAN list will be disabled\n").c_str());
            return;
        }

        UDPpacket* pkt = SDLNet_AllocPacket(1200);
        if (!pkt)
        {
            SDLNet_UDP_Close(sock);
            DebugLog("LAN discovery: could not allocate UDP packet\n");
            return;
        }

        DebugLog(("LAN discovery: responder listening on UDP " +
                  std::to_string(kLanDiscoveryPort) + "\n").c_str());

        while (!g_lanStop.load())
        {
            while (SDLNet_UDP_Recv(sock, pkt) > 0)
            {
                std::string text(reinterpret_cast<char*>(pkt->data), static_cast<size_t>(pkt->len));
                Json::Value req;
                if (!parseJson(text, req))
                    continue;
                if (req.get("type", "").asString() != kDiscoverType)
                    continue;
                if (req.get("version", 0).asUInt() != kLanProtocolVersion)
                    continue;

                RendezvousClient::RoomInfo room;
                {
                    std::lock_guard<std::mutex> lock(g_lanMutex);
                    if (!g_lanRoomValid)
                        continue;
                    room = g_lanRoom;
                }

                const std::string requestedVersion = req.get("game_version", "").asString();
                const std::string requestedModHash = req.get("mod_hash", "").asString();
                if (!requestedVersion.empty() && !room.gameVersion.empty() && requestedVersion != room.gameVersion)
                    continue;
                if (!requestedModHash.empty() && !room.modHash.empty() && requestedModHash != room.modHash)
                    continue;

                Json::Value out;
                out["type"] = kRoomType;
                out["version"] = kLanProtocolVersion;
                out["room_id"] = room.roomId;
                out["room_name"] = room.name;
                out["host_name"] = room.hostName;
                out["region"] = room.region.empty() ? "LAN" : room.region;
                out["players"] = Json::UInt(room.players);
                out["max_players"] = Json::UInt(room.maxPlayers);
                out["password_required"] = room.passwordRequired;
                out["is_campaign"] = room.isCampaign;
                out["game_version"] = room.gameVersion;
                out["mod_hash"] = room.modHash;
                out["udp_port"] = Json::UInt(room.lanPort);

                const std::string payload = compactJson(out);
                if (payload.size() > 1000)
                    continue;

                pkt->len = static_cast<int>(payload.size());
                std::memcpy(pkt->data, payload.data(), payload.size());
                // Send the response directly to the requester. The requester
                // reads the source IP from the packet and uses that as lanHost.
                SDLNet_UDP_Send(sock, -1, pkt);
            }
            SDL_Delay(10);
        }

        SDLNet_FreePacket(pkt);
        SDLNet_UDP_Close(sock);
        DebugLog("LAN discovery: responder stopped\n");
    }
}

void startLanDiscoveryHost(const RendezvousClient::RoomInfo& room)
{
    {
        std::lock_guard<std::mutex> lock(g_lanMutex);
        g_lanRoom = room;
        g_lanRoom.isLan = true;
        if (g_lanRoom.region.empty())
            g_lanRoom.region = "LAN";
        g_lanRoomValid = !g_lanRoom.roomId.empty() && g_lanRoom.lanPort != 0;
    }

    if (!g_lanRoomValid)
    {
        DebugLog("LAN discovery: not started because roomId or lanPort is missing\n");
        return;
    }

    if (g_lanThread.joinable())
        return;

    g_lanStop.store(false);
    g_lanThread = std::thread(lanResponderThread);
}

void stopLanDiscoveryHost()
{
    {
        std::lock_guard<std::mutex> lock(g_lanMutex);
        g_lanRoomValid = false;
    }

    g_lanStop.store(true);
    if (g_lanThread.joinable())
        g_lanThread.join();
}

void refreshLanServerList(std::vector<RendezvousClient::RoomInfo>& ioRooms,
                          const std::string& gameVersion,
                          const std::string& modHash,
                          bool compatibleOnly,
                          uint32_t timeoutMs)
{
    if (SDLNet_Init() == -1)
        return;

    UDPsocket sock = SDLNet_UDP_Open(0);
    if (!sock)
        return;

    UDPpacket* pkt = SDLNet_AllocPacket(1200);
    if (!pkt)
    {
        SDLNet_UDP_Close(sock);
        return;
    }

    IPaddress broadcast{};
    if (SDLNet_ResolveHost(&broadcast, "255.255.255.255", kLanDiscoveryPort) == -1)
    {
        SDLNet_FreePacket(pkt);
        SDLNet_UDP_Close(sock);
        return;
    }

    Json::Value req;
    req["type"] = kDiscoverType;
    req["version"] = kLanProtocolVersion;
    req["game_version"] = gameVersion;
    req["mod_hash"] = modHash;

    const std::string payload = compactJson(req);
    pkt->address = broadcast;
    pkt->len = static_cast<int>(payload.size());
    std::memcpy(pkt->data, payload.data(), payload.size());
    SDLNet_UDP_Send(sock, -1, pkt);

    const uint64_t start = static_cast<uint64_t>(SDL_GetTicks());
    while (static_cast<uint64_t>(SDL_GetTicks()) - start < timeoutMs)
    {
        while (SDLNet_UDP_Recv(sock, pkt) > 0)
        {
            std::string text(reinterpret_cast<char*>(pkt->data), static_cast<size_t>(pkt->len));
            Json::Value msg;
            if (!parseJson(text, msg))
                continue;
            if (msg.get("type", "").asString() != kRoomType)
                continue;
            if (msg.get("version", 0).asUInt() != kLanProtocolVersion)
                continue;

            RendezvousClient::RoomInfo room;
            if (!roomFromLanResponse(msg, pkt->address, gameVersion, modHash, compatibleOnly, room))
                continue;

            mergeRoom(ioRooms, room);
        }
        SDL_Delay(10);
    }

    SDLNet_FreePacket(pkt);
    SDLNet_UDP_Close(sock);
}

bool findLanRoomByAddress(const std::string& hostLanIp,
                          RendezvousClient::RoomInfo& outRoom,
                          const std::string& gameVersion,
                          const std::string& modHash,
                          bool compatibleOnly,
                          uint32_t timeoutMs)
{
    outRoom = RendezvousClient::RoomInfo();

    if (hostLanIp.empty())
        return false;

    if (SDLNet_Init() == -1)
        return false;

    UDPsocket sock = SDLNet_UDP_Open(0);
    if (!sock)
        return false;

    UDPpacket* pkt = SDLNet_AllocPacket(1200);
    if (!pkt)
    {
        SDLNet_UDP_Close(sock);
        return false;
    }

    IPaddress target{};
    if (SDLNet_ResolveHost(&target, hostLanIp.c_str(), kLanDiscoveryPort) == -1)
    {
        SDLNet_FreePacket(pkt);
        SDLNet_UDP_Close(sock);
        return false;
    }

    Json::Value req;
    req["type"] = kDiscoverType;
    req["version"] = kLanProtocolVersion;
    req["game_version"] = gameVersion;
    req["mod_hash"] = modHash;

    const std::string payload = compactJson(req);
    pkt->address = target;
    pkt->len = static_cast<int>(payload.size());
    std::memcpy(pkt->data, payload.data(), payload.size());

    const uint64_t start = static_cast<uint64_t>(SDL_GetTicks());
    uint64_t lastSend = 0;

    while (static_cast<uint64_t>(SDL_GetTicks()) - start < timeoutMs)
    {
        const uint64_t now = static_cast<uint64_t>(SDL_GetTicks());
        if (lastSend == 0 || now - lastSend >= 100)
        {
            SDLNet_UDP_Send(sock, -1, pkt);
            lastSend = now;
        }

        while (SDLNet_UDP_Recv(sock, pkt) > 0)
        {
            Json::Value msg;
            const std::string text(reinterpret_cast<char*>(pkt->data), static_cast<size_t>(pkt->len));
            if (!parseJson(text, msg))
                continue;

            RendezvousClient::RoomInfo room;
            if (!roomFromLanResponse(msg, pkt->address, gameVersion, modHash, compatibleOnly, room))
                continue;

            outRoom = room;
            SDLNet_FreePacket(pkt);
            SDLNet_UDP_Close(sock);
            return true;
        }

        SDL_Delay(10);
    }

    SDLNet_FreePacket(pkt);
    SDLNet_UDP_Close(sock);
    return false;
}

}
