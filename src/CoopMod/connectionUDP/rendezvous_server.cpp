/*
 * Secure rendezvous + server-list server for OpenXcom UDP transport.
 *
 * Features:
 *  - Encrypted/signed TCP control protocol using libsodium sealed boxes + signatures.
 *  - Public server list: clients can query listed rooms without seeing host IP/port or secrets.
 *  - CREATE_ROOM / JOIN_ROOM flow for UX-friendly lobby browser.
 *  - Legacy JOIN compatibility: if room does not exist it is created, otherwise joined.
 *  - UDP_REGISTER over UDP to discover each player's public UDP endpoint.
 *  - PEER_READY contains peer endpoint + generated P2P session key only after both players are authenticated.
 *
 * Build example:
 *   g++ -std=c++11 rendezvous_server.cpp -o rendezvous_server \
 *       -lSDL_net -lSDL -lsodium -ljsoncpp -pthread
 *
 * Usage:
 *   mkdir -p rv_keys
 *   ./rendezvous_server --generate-keys ./rv_keys
 *   ./rendezvous_server --tcp 39000 --udp 39001 --keys ./rv_keys
 */
#ifdef BUILD_RENDEZVOUS_SERVER
#include <SDL_net.h>
#include <json/json.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    static const uint32_t kProtocolVersion = 2;
    static const size_t kMaxFrame = 64 * 1024;
    static const size_t kUdpRegisterTokenBytes = 32;
    static const uint64_t kRoomTtlMs = 0; // 0 = no waiting-room timeout; TCP disconnect still removes stale rooms.
    static const uint32_t kMaxRoomsReturned = 128;

    struct ServerKeys
    {
        std::array<unsigned char, crypto_box_PUBLICKEYBYTES> boxPk{};
        std::array<unsigned char, crypto_box_SECRETKEYBYTES> boxSk{};
        std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> signPk{};
        std::array<unsigned char, crypto_sign_SECRETKEYBYTES> signSk{};
    };

    struct Player
    {
        uint32_t playerId = 0;
        std::string playerName;
        std::array<unsigned char, crypto_box_PUBLICKEYBYTES> clientBoxPk{};
        std::array<unsigned char, kUdpRegisterTokenBytes> udpToken{};
        TCPsocket tcp = nullptr;
        std::mutex tcpMutex;
        bool udpRegistered = false;
        IPaddress udpAddress{};
        bool peerReadySent = false;
        bool isHost = false;
        uint64_t joinedAtMs = 0;
    };

    struct Room
    {
        std::string roomId;
        std::string roomName;
        std::string hostName;
        std::string region;
        std::string gameVersion;
        std::string modHash;
        bool isCampaign = false;

        bool listed = true;
        bool passwordRequired = false;
        bool locked = false;
        bool closed = false;

        std::array<unsigned char, crypto_generichash_BYTES> passwordHash{};
        std::array<unsigned char, crypto_generichash_BYTES> hostTokenHash{};

        uint64_t sessionId = 0;
        uint32_t desiredPlayers = 2;
        std::vector<std::shared_ptr<Player>> players;
        std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_KEYBYTES> sessionKey{};
        bool sessionKeyReady = false;

        uint64_t createdAtMs = 0;
        uint64_t lastHeartbeatMs = 0;
    };

    std::mutex g_roomsMutex;
    std::map<std::string, std::shared_ptr<Room>> g_rooms;
    std::atomic<bool> g_stop(false);
    ServerKeys g_keys;

    uint64_t nowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    void writeBE32(unsigned char* p, uint32_t v)
    {
        p[0] = static_cast<unsigned char>((v >> 24) & 0xff);
        p[1] = static_cast<unsigned char>((v >> 16) & 0xff);
        p[2] = static_cast<unsigned char>((v >> 8) & 0xff);
        p[3] = static_cast<unsigned char>(v & 0xff);
    }

    uint32_t readBE32(const unsigned char* p)
    {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    }

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

    std::string b64(const unsigned char* data, size_t n)
    {
        const size_t maxLen = sodium_base64_ENCODED_LEN(n, sodium_base64_VARIANT_ORIGINAL);
        std::string out(maxLen, '\0');
        sodium_bin2base64(&out[0], out.size(), data, n, sodium_base64_VARIANT_ORIGINAL);
        out.resize(std::strlen(out.c_str()));
        return out;
    }

    bool unb64(const std::string& s, std::vector<unsigned char>& out, size_t expected = 0)
    {
        out.resize(s.size());
        size_t binLen = 0;
        if (sodium_base642bin(out.data(), out.size(), s.c_str(), s.size(), nullptr,
                              &binLen, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0)
            return false;
        out.resize(binLen);
        return expected == 0 || binLen == expected;
    }

    bool saveBin(const std::string& path, const unsigned char* data, size_t n)
    {
        std::ofstream f(path.c_str(), std::ios::binary);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
        return !!f;
    }

    bool loadBin(const std::string& path, unsigned char* data, size_t n)
    {
        std::ifstream f(path.c_str(), std::ios::binary);
        if (!f) return false;
        f.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(n));
        return f.gcount() == static_cast<std::streamsize>(n);
    }

    bool generateKeys(const std::string& dir)
    {
        ServerKeys k;
        crypto_box_keypair(k.boxPk.data(), k.boxSk.data());
        crypto_sign_keypair(k.signPk.data(), k.signSk.data());

        const std::string slash = dir.empty() || dir[dir.size() - 1] == '/' ? "" : "/";
        bool ok = true;
        ok = ok && saveBin(dir + slash + "server_box_public.key", k.boxPk.data(), k.boxPk.size());
        ok = ok && saveBin(dir + slash + "server_box_secret.key", k.boxSk.data(), k.boxSk.size());
        ok = ok && saveBin(dir + slash + "server_sign_public.key", k.signPk.data(), k.signPk.size());
        ok = ok && saveBin(dir + slash + "server_sign_secret.key", k.signSk.data(), k.signSk.size());
        if (!ok)
            std::cerr << "Could not write all key files. Create directory first: " << dir << "\n";
        return ok;
    }

    bool loadKeys(const std::string& dir, ServerKeys& k)
    {
        const std::string slash = dir.empty() || dir[dir.size() - 1] == '/' ? "" : "/";
        return loadBin(dir + slash + "server_box_public.key", k.boxPk.data(), k.boxPk.size()) &&
               loadBin(dir + slash + "server_box_secret.key", k.boxSk.data(), k.boxSk.size()) &&
               loadBin(dir + slash + "server_sign_public.key", k.signPk.data(), k.signPk.size()) &&
               loadBin(dir + slash + "server_sign_secret.key", k.signSk.data(), k.signSk.size());
    }

    bool sendAll(TCPsocket sock, const unsigned char* data, size_t len)
    {
        size_t sent = 0;
        while (sent < len)
        {
            const int n = SDLNet_TCP_Send(sock, data + sent, static_cast<int>(len - sent));
            if (n <= 0)
                return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool sendFrame(TCPsocket sock, const std::string& payload)
    {
        if (!sock || payload.empty() || payload.size() > kMaxFrame)
            return false;
        unsigned char len[4];
        writeBE32(len, static_cast<uint32_t>(payload.size()));
        return sendAll(sock, len, 4) &&
               sendAll(sock, reinterpret_cast<const unsigned char*>(payload.data()), payload.size());
    }

    bool recvExact(TCPsocket sock, unsigned char* data, size_t len)
    {
        size_t got = 0;
        while (got < len)
        {
            const int n = SDLNet_TCP_Recv(sock, data + got, static_cast<int>(len - got));
            if (n <= 0)
                return false;
            got += static_cast<size_t>(n);
        }
        return true;
    }

    bool recvFrame(TCPsocket sock, std::string& out)
    {
        unsigned char lenBuf[4];
        if (!recvExact(sock, lenBuf, 4))
            return false;
        const uint32_t len = readBE32(lenBuf);
        if (len == 0 || len > kMaxFrame)
            return false;
        out.resize(len);
        return recvExact(sock, reinterpret_cast<unsigned char*>(&out[0]), len);
    }

    bool sealToClient(const Json::Value& plainWithoutSig,
                      const unsigned char* clientPk,
                      std::string& outerJson)
    {
        Json::Value signedPlain = plainWithoutSig;
        const std::string payload = compactJson(plainWithoutSig);
        unsigned char sig[crypto_sign_BYTES];
        crypto_sign_detached(sig, nullptr,
                             reinterpret_cast<const unsigned char*>(payload.data()),
                             static_cast<unsigned long long>(payload.size()),
                             g_keys.signSk.data());
        signedPlain["server_sig"] = b64(sig, sizeof(sig));

        const std::string signedPayload = compactJson(signedPlain);
        std::string sealed;
        sealed.resize(crypto_box_SEALBYTES + signedPayload.size());
        if (crypto_box_seal(reinterpret_cast<unsigned char*>(&sealed[0]),
                            reinterpret_cast<const unsigned char*>(signedPayload.data()),
                            static_cast<unsigned long long>(signedPayload.size()),
                            clientPk) != 0)
            return false;

        Json::Value outer;
        outer["type"] = "SERVER_MSG";
        outer["version"] = kProtocolVersion;
        outer["sealed"] = b64(reinterpret_cast<const unsigned char*>(sealed.data()), sealed.size());
        outerJson = compactJson(outer);
        return true;
    }

    bool sendServerMsgToSocket(TCPsocket sock, const unsigned char* clientPk, const Json::Value& msg)
    {
        std::string outer;
        if (!sealToClient(msg, clientPk, outer))
            return false;
        return sendFrame(sock, outer);
    }

    bool sendServerMsg(const std::shared_ptr<Player>& p, const Json::Value& msg)
    {
        if (!p || !p->tcp)
            return false;
        std::string outer;
        if (!sealToClient(msg, p->clientBoxPk.data(), outer))
            return false;
        std::lock_guard<std::mutex> lock(p->tcpMutex);
        return sendFrame(p->tcp, outer);
    }

    bool sendError(TCPsocket sock, const unsigned char* clientPk, const std::string& message)
    {
        Json::Value msg;
        msg["kind"] = "ERROR";
        msg["message"] = message;
        return sendServerMsgToSocket(sock, clientPk, msg);
    }

    void hashBytes(const std::string& data, std::array<unsigned char, crypto_generichash_BYTES>& out)
    {
        crypto_generichash(out.data(), out.size(),
                           reinterpret_cast<const unsigned char*>(data.data()),
                           static_cast<unsigned long long>(data.size()),
                           nullptr, 0);
    }

    std::string ipToString(const IPaddress& a)
    {
        const Uint32 host = SDL_SwapBE32(a.host);
        std::ostringstream ss;
        ss << ((host >> 24) & 0xff) << "."
           << ((host >> 16) & 0xff) << "."
           << ((host >> 8) & 0xff) << "."
           << (host & 0xff);
        return ss.str();
    }

    uint16_t portHostOrder(const IPaddress& a)
    {
        return SDL_SwapBE16(a.port);
    }

    uint64_t randomSessionId()
    {
        uint64_t v = 0;
        randombytes_buf(&v, sizeof(v));
        if (v == 0) v = 1;
        return v;
    }

    std::string randomTokenB64(size_t bytes)
    {
        std::vector<unsigned char> tmp(bytes);
        randombytes_buf(tmp.data(), tmp.size());
        return b64(tmp.data(), tmp.size());
    }

    std::string randomRoomId()
    {
        unsigned char tmp[9];
        randombytes_buf(tmp, sizeof(tmp));
        std::string s = b64(tmp, sizeof(tmp));
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '+') s[i] = 'A';
            if (s[i] == '/') s[i] = 'B';
            if (s[i] == '=') s.resize(i);
        }
        return s;
    }

    bool passwordsMatch(const Room& room, const std::string& password)
    {
        if (!room.passwordRequired)
            return true;
        std::array<unsigned char, crypto_generichash_BYTES> pwHash{};
        hashBytes(password, pwHash);
        return sodium_memcmp(room.passwordHash.data(), pwHash.data(), pwHash.size()) == 0;
    }

    bool hostTokenMatches(const Room& room, const std::string& token)
    {
        std::array<unsigned char, crypto_generichash_BYTES> h{};
        hashBytes(token, h);
        return sodium_memcmp(room.hostTokenHash.data(), h.data(), h.size()) == 0;
    }

    Json::Value publicRoomJson(const Room& room)
    {
        Json::Value r;
        r["room_id"] = room.roomId;
        r["name"] = room.roomName;
        r["host_name"] = room.hostName;
        r["region"] = room.region;
        r["players"] = Json::UInt(room.players.size());
        r["max_players"] = Json::UInt(room.desiredPlayers);
        r["locked"] = room.locked || room.sessionKeyReady || room.closed;
        r["password_required"] = room.passwordRequired;
        r["is_campaign"] = room.isCampaign;
        r["game_version"] = room.gameVersion;
        r["mod_hash"] = room.modHash;
        r["created_at_ms"] = std::to_string(room.createdAtMs);
        return r;
    }

    void maybeFinishRoomLocked(const std::shared_ptr<Room>& room)
    {
        if (!room || room->players.size() < room->desiredPlayers)
            return;

        for (const auto& p : room->players)
        {
            if (!p->udpRegistered)
                return;
        }

        if (!room->sessionKeyReady)
        {
            randombytes_buf(room->sessionKey.data(), room->sessionKey.size());
            room->sessionKeyReady = true;
            // Do not use a timer to close the room while the lobby waits.
            // The room is no longer returned by LIST_ROOMS because it is full
            // and sessionKeyReady is true, but the control connection can stay
            // alive until the game/lobby closes it.
            room->lastHeartbeatMs = nowMs();
        }

        if (room->players.size() != 2)
        {
            // This implementation is intentionally two-player first.
            return;
        }

        auto a = room->players[0];
        auto b = room->players[1];
        if (a->peerReadySent && b->peerReadySent)
            return;

        Json::Value ma;
        ma["kind"] = "PEER_READY";
        ma["session_id"] = std::to_string(room->sessionId);
        ma["session_key"] = b64(room->sessionKey.data(), room->sessionKey.size());
        ma["remote_host"] = ipToString(b->udpAddress);
        ma["remote_port"] = Json::UInt(portHostOrder(b->udpAddress));
        ma["remote_player_id"] = Json::UInt(b->playerId);
        ma["peer_player_name"] = b->playerName;

        Json::Value mb;
        mb["kind"] = "PEER_READY";
        mb["session_id"] = std::to_string(room->sessionId);
        mb["session_key"] = b64(room->sessionKey.data(), room->sessionKey.size());
        mb["remote_host"] = ipToString(a->udpAddress);
        mb["remote_port"] = Json::UInt(portHostOrder(a->udpAddress));
        mb["remote_player_id"] = Json::UInt(a->playerId);
        mb["peer_player_name"] = a->playerName;

        /*
         * Send PEER_READY only to players that have not received it yet.
         *
         * Important:
         * Do not mark peerReadySent=true before the TCP send has actually
         * succeeded. The previous detached-thread version marked both players
         * as sent immediately, so if one TCP send failed or did not leave the
         * socket cleanly, the server stopped retrying and that client could
         * remain stuck at "JOIN_ROOM_OK, waiting for peer".
         *
         * UDP_REGISTER is sent repeatedly by the clients while they are waiting,
         * so leaving peerReadySent=false on failure lets the next UDP_REGISTER
         * call maybeFinishRoomLocked(...) again and retry the missing side.
         */
        if (!a->peerReadySent)
        {
            const bool sentA = sendServerMsg(a, ma);
            if (sentA)
            {
                a->peerReadySent = true;
                std::cout << "PEER_READY sent to player "
                          << a->playerName
                          << " room=" << room->roomId << "\n";
            }
            else
            {
                std::cout << "PEER_READY send failed for player "
                          << a->playerName
                          << " room=" << room->roomId << "\n";
            }
        }

        if (!b->peerReadySent)
        {
            const bool sentB = sendServerMsg(b, mb);
            if (sentB)
            {
                b->peerReadySent = true;
                std::cout << "PEER_READY sent to player "
                          << b->playerName
                          << " room=" << room->roomId << "\n";
            }
            else
            {
                std::cout << "PEER_READY send failed for player "
                          << b->playerName
                          << " room=" << room->roomId << "\n";
            }
        }

        if (a->peerReadySent && b->peerReadySent)
        {
            std::cout << "Room " << room->roomId << " ready: "
                      << a->playerName << " <-> " << b->playerName << "\n";
        }
    }

    bool decryptClientFrame(const std::string& frame,
                            std::array<unsigned char, crypto_box_PUBLICKEYBYTES>& clientPk,
                            Json::Value& plain,
                            std::string& error)
    {
        Json::Value outer;
        if (!parseJson(frame, outer))
        {
            error = "bad json outer";
            return false;
        }
        const std::string type = outer.get("type", "").asString();
        if (type != "CLIENT_MSG" && type != "JOIN")
        {
            error = "unsupported outer type";
            return false;
        }

        std::vector<unsigned char> clientPkVec;
        std::vector<unsigned char> sealedVec;
        if (!unb64(outer.get("client_box_pk", "").asString(), clientPkVec, crypto_box_PUBLICKEYBYTES) ||
            !unb64(outer.get("sealed", "").asString(), sealedVec))
        {
            error = "bad base64";
            return false;
        }
        std::copy(clientPkVec.begin(), clientPkVec.end(), clientPk.begin());

        if (sealedVec.size() < crypto_box_SEALBYTES)
        {
            error = "sealed box too small";
            return false;
        }
        std::string plainStr(sealedVec.size() - crypto_box_SEALBYTES, '\0');
        if (crypto_box_seal_open(reinterpret_cast<unsigned char*>(&plainStr[0]),
                                 sealedVec.data(),
                                 static_cast<unsigned long long>(sealedVec.size()),
                                 g_keys.boxPk.data(),
                                 g_keys.boxSk.data()) != 0)
        {
            error = "could not decrypt sealed client message";
            return false;
        }
        if (!parseJson(plainStr, plain))
        {
            error = "bad json payload";
            return false;
        }
        return true;
    }

    std::shared_ptr<Player> makePlayer(const Json::Value& msg,
                                       const std::array<unsigned char, crypto_box_PUBLICKEYBYTES>& clientPk,
                                       TCPsocket sock,
                                       bool isHost)
    {
        std::shared_ptr<Player> p(new Player());
        p->playerName = msg.get("player_name", "Player").asString();
        if (p->playerName.empty()) p->playerName = "Player";
        if (p->playerName.size() > 32) p->playerName.resize(32);
        p->clientBoxPk = clientPk;
        p->tcp = sock;
        p->isHost = isHost;
        p->joinedAtMs = nowMs();
        randombytes_buf(p->udpToken.data(), p->udpToken.size());
        return p;
    }

    void sendJoinAccepted(const std::shared_ptr<Player>& p,
                          const std::shared_ptr<Room>& room,
                          const std::string& kind,
                          const std::string& hostToken = std::string())
    {
        Json::Value ok;
        ok["kind"] = kind;
        ok["room_id"] = room->roomId;
        ok["session_id"] = std::to_string(room->sessionId);
        ok["player_id"] = Json::UInt(p->playerId);
        ok["desired_players"] = Json::UInt(room->desiredPlayers);
        ok["udp_token"] = b64(p->udpToken.data(), p->udpToken.size());
        if (!hostToken.empty())
            ok["host_token"] = hostToken;
        sendServerMsg(p, ok);

        Json::Value waiting;
        waiting["kind"] = "WAITING";
        waiting["message"] = "waiting for peer UDP registration";
        sendServerMsg(p, waiting);
    }

    void handleListRooms(TCPsocket sock,
                         const std::array<unsigned char, crypto_box_PUBLICKEYBYTES>& clientPk,
                         const Json::Value& req)
    {
        std::string region = req.get("region", "").asString();
        if (region.size() > 32) region.resize(32);
        const std::string gameVersion = req.get("game_version", "").asString();
        const std::string modHash = req.get("mod_hash", "").asString();
        const bool compatibleOnly = req.get("compatible_only", false).asBool();

        Json::Value out;
        out["kind"] = "ROOM_LIST";
        out["rooms"] = Json::arrayValue;

        std::lock_guard<std::mutex> lock(g_roomsMutex);
        uint32_t count = 0;
        for (const auto& kv : g_rooms)
        {
            const Room& r = *kv.second;
            if (!r.listed || r.closed || r.locked || r.sessionKeyReady)
                continue;
            if (r.players.size() >= r.desiredPlayers)
                continue;
            if (compatibleOnly)
            {
                if (!gameVersion.empty() && r.gameVersion != gameVersion)
                    continue;
                if (!modHash.empty() && r.modHash != modHash)
                    continue;
            }
            out["rooms"].append(publicRoomJson(r));
            if (++count >= kMaxRoomsReturned)
                break;
        }
        sendServerMsgToSocket(sock, clientPk.data(), out);
    }

    bool handleCreateRoom(TCPsocket sock,
                          const std::array<unsigned char, crypto_box_PUBLICKEYBYTES>& clientPk,
                          const Json::Value& req,
                          std::shared_ptr<Room>& outRoom,
                          std::shared_ptr<Player>& outPlayer)
    {
        std::string roomName = req.get("room_name", "Game").asString();
        if (roomName.empty()) roomName = "Game";
        if (roomName.size() > 48) roomName.resize(48);

        const std::string password = req.get("password", "").asString();
        const bool listed = req.get("listed", true).asBool();
        const bool passwordRequired = req.get("password_required", !password.empty()).asBool();
        const uint32_t desiredPlayers = std::max<uint32_t>(2, std::min<uint32_t>(4, req.get("desired_players", 2).asUInt()));
        std::string region = req.get("region", "").asString();
        if (region.size() > 32) region.resize(32);
        const std::string gameVersion = req.get("game_version", "").asString();
        const std::string modHash = req.get("mod_hash", "").asString();
        const bool isCampaign = req.get("is_campaign", false).asBool();

        std::string roomId;
        std::string hostToken = randomTokenB64(32);
        std::shared_ptr<Room> room(new Room());
        room->roomName = roomName;
        room->hostName = req.get("player_name", "Host").asString();
        room->region = region;
        if (room->hostName.empty()) room->hostName = "Host";
        if (room->hostName.size() > 32) room->hostName.resize(32);
        room->listed = listed;
        room->passwordRequired = passwordRequired;
        room->isCampaign = isCampaign;
        room->gameVersion = gameVersion;
        room->modHash = modHash;
        room->sessionId = randomSessionId();
        room->desiredPlayers = desiredPlayers;
        room->createdAtMs = nowMs();
        room->lastHeartbeatMs = room->createdAtMs;
        hashBytes(password, room->passwordHash);
        hashBytes(hostToken, room->hostTokenHash);

        std::shared_ptr<Player> host = makePlayer(req, clientPk, sock, true);
        host->playerId = 1;
        room->players.push_back(host);

        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            do
            {
                roomId = randomRoomId();
            } while (g_rooms.find(roomId) != g_rooms.end());
            room->roomId = roomId;
            g_rooms[roomId] = room;
        }

        outRoom = room;
        outPlayer = host;
        sendJoinAccepted(host, room, "CREATE_ROOM_OK", hostToken);
        std::cout << "Created room " << room->roomId << " name='" << room->roomName
                  << "' host='" << room->hostName << "' listed=" << (room->listed ? "yes" : "no") << "\n";
        return true;
    }

    bool handleJoinRoom(TCPsocket sock,
                        const std::array<unsigned char, crypto_box_PUBLICKEYBYTES>& clientPk,
                        const Json::Value& req,
                        std::shared_ptr<Room>& outRoom,
                        std::shared_ptr<Player>& outPlayer)
    {
        std::string roomId = req.get("room_id", "").asString();
        if (roomId.empty())
            roomId = req.get("room", "").asString();
        const std::string password = req.get("password", "").asString();
        if (roomId.empty())
        {
            sendError(sock, clientPk.data(), "room id missing");
            return false;
        }

        std::shared_ptr<Room> room;
        std::shared_ptr<Player> p = makePlayer(req, clientPk, sock, false);
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            auto it = g_rooms.find(roomId);
            if (it == g_rooms.end())
            {
                sendError(sock, clientPk.data(), "room not found");
                return false;
            }
            room = it->second;
            if (room->closed || room->locked || room->sessionKeyReady)
            {
                sendError(sock, clientPk.data(), "room locked");
                return false;
            }
            if (room->players.size() >= room->desiredPlayers)
            {
                sendError(sock, clientPk.data(), "room full");
                return false;
            }
            if (!passwordsMatch(*room, password))
            {
                sendError(sock, clientPk.data(), "wrong room password");
                return false;
            }

            const std::string requestedGameVersion = req.get("game_version", "").asString();
            const std::string requestedModHash = req.get("mod_hash", "").asString();
            if (!requestedGameVersion.empty() && !room->gameVersion.empty() && requestedGameVersion != room->gameVersion)
            {
                sendError(sock, clientPk.data(), "incompatible mods");
                return false;
            }
            if (!requestedModHash.empty() && !room->modHash.empty() && requestedModHash != room->modHash)
            {
                sendError(sock, clientPk.data(), "incompatible mods");
                return false;
            }

            p->playerId = static_cast<uint32_t>(room->players.size() + 1);
            room->players.push_back(p);
            room->lastHeartbeatMs = nowMs();
        }

        outRoom = room;
        outPlayer = p;
        sendJoinAccepted(p, room, "JOIN_ROOM_OK");
        std::cout << "Player '" << p->playerName << "' joined room " << room->roomId << "\n";
        return true;
    }

    bool handleLegacyJoin(TCPsocket sock,
                          const std::array<unsigned char, crypto_box_PUBLICKEYBYTES>& clientPk,
                          const Json::Value& req,
                          std::shared_ptr<Room>& outRoom,
                          std::shared_ptr<Player>& outPlayer)
    {
        const std::string roomId = req.get("room", "").asString();
        const std::string password = req.get("password", "").asString();
        const uint32_t desiredPlayers = std::max<uint32_t>(2, std::min<uint32_t>(4, req.get("desired_players", 2).asUInt()));
        if (roomId.empty() || password.empty())
        {
            sendError(sock, clientPk.data(), "room id or password missing");
            return false;
        }

        std::shared_ptr<Room> room;
        std::shared_ptr<Player> p;
        std::string hostToken;
        bool created = false;
        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            auto it = g_rooms.find(roomId);
            if (it == g_rooms.end())
            {
                created = true;
                hostToken = randomTokenB64(32);
                room.reset(new Room());
                room->roomId = roomId;
                room->roomName = req.get("room_name", "Private Game").asString();
                room->hostName = req.get("player_name", "Host").asString();
                room->region = req.get("region", "").asString();
                if (room->region.size() > 32) room->region.resize(32);
                room->listed = false;
                room->passwordRequired = true;
                room->sessionId = randomSessionId();
                room->desiredPlayers = desiredPlayers;
                room->createdAtMs = nowMs();
                room->lastHeartbeatMs = room->createdAtMs;
                room->gameVersion = req.get("game_version", "").asString();
                room->modHash = req.get("mod_hash", "").asString();
                hashBytes(password, room->passwordHash);
                hashBytes(hostToken, room->hostTokenHash);
                g_rooms[roomId] = room;
            }
            else
            {
                room = it->second;
                if (!passwordsMatch(*room, password))
                {
                    sendError(sock, clientPk.data(), "wrong room password");
                    return false;
                }

                const std::string requestedGameVersion = req.get("game_version", "").asString();
                const std::string requestedModHash = req.get("mod_hash", "").asString();
                if (!requestedGameVersion.empty() && !room->gameVersion.empty() && requestedGameVersion != room->gameVersion)
                {
                    sendError(sock, clientPk.data(), "incompatible mods");
                    return false;
                }
                if (!requestedModHash.empty() && !room->modHash.empty() && requestedModHash != room->modHash)
                {
                    sendError(sock, clientPk.data(), "incompatible mods");
                    return false;
                }

                if (room->players.size() >= room->desiredPlayers || room->sessionKeyReady || room->locked)
                {
                    sendError(sock, clientPk.data(), "room is full or already locked");
                    return false;
                }
            }
            p = makePlayer(req, clientPk, sock, created);
            p->playerId = static_cast<uint32_t>(room->players.size() + 1);
            room->players.push_back(p);
            room->lastHeartbeatMs = nowMs();
        }
        outRoom = room;
        outPlayer = p;
        sendJoinAccepted(p, room, created ? "CREATE_ROOM_OK" : "JOIN_OK", hostToken);
        return true;
    }

    bool handleRoomHeartbeatOrClose(TCPsocket sock,
                                    const std::array<unsigned char, crypto_box_PUBLICKEYBYTES>& clientPk,
                                    const Json::Value& req,
                                    bool closeRoom)
    {
        const std::string roomId = req.get("room_id", "").asString();
        const std::string hostToken = req.get("host_token", "").asString();
        if (roomId.empty() || hostToken.empty())
        {
            sendError(sock, clientPk.data(), "room id or host token missing");
            return false;
        }

        std::lock_guard<std::mutex> lock(g_roomsMutex);
        auto it = g_rooms.find(roomId);
        if (it == g_rooms.end())
        {
            sendError(sock, clientPk.data(), "room not found");
            return false;
        }
        auto room = it->second;
        if (!hostTokenMatches(*room, hostToken))
        {
            sendError(sock, clientPk.data(), "bad host token");
            return false;
        }
        if (closeRoom)
        {
            // The host sends CLOSE_ROOM only after the game lobby reports
            // connectionTCP::session.sessionLocked == true, meaning all players
            // clicked Ready and the actual game session is locked.
            room->closed = true;
            room->locked = true;
            room->listed = false;
            room->lastHeartbeatMs = nowMs();
        }
        else
        {
            room->lastHeartbeatMs = nowMs();
        }
        Json::Value ok;
        ok["kind"] = closeRoom ? "CLOSE_ROOM_OK" : "ROOM_HEARTBEAT_OK";
        sendServerMsgToSocket(sock, clientPk.data(), ok);
        return true;
    }

    void keepControlConnectionAlive(const std::shared_ptr<Room>& room,
                                    const std::shared_ptr<Player>& player)
    {
        while (!g_stop.load())
        {
            std::string ignored;
            if (!recvFrame(player->tcp, ignored))
                break;
            if (room)
            {
                std::lock_guard<std::mutex> lock(g_roomsMutex);
                room->lastHeartbeatMs = nowMs();
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_roomsMutex);
            if (room && !room->sessionKeyReady)
            {
                room->players.erase(std::remove(room->players.begin(), room->players.end(), player), room->players.end());
                if (room->players.empty())
                    g_rooms.erase(room->roomId);
                else
                    room->lastHeartbeatMs = nowMs();
            }
        }
        SDLNet_TCP_Close(player->tcp);
        player->tcp = nullptr;
    }

    void clientThread(TCPsocket sock)
    {
        std::string frame;
        if (!recvFrame(sock, frame))
        {
            SDLNet_TCP_Close(sock);
            return;
        }

        std::array<unsigned char, crypto_box_PUBLICKEYBYTES> clientPk{};
        Json::Value req;
        std::string err;
        if (!decryptClientFrame(frame, clientPk, req, err))
        {
            std::cerr << "Bad client frame: " << err << "\n";
            SDLNet_TCP_Close(sock);
            return;
        }

        const std::string kind = req.get("kind", req.isMember("room") ? "JOIN" : "").asString();
        std::shared_ptr<Room> room;
        std::shared_ptr<Player> player;

        if (kind == "LIST_ROOMS")
        {
            handleListRooms(sock, clientPk, req);
            SDLNet_TCP_Close(sock);
            return;
        }
        if (kind == "CREATE_ROOM")
        {
            if (!handleCreateRoom(sock, clientPk, req, room, player))
            {
                SDLNet_TCP_Close(sock);
                return;
            }
            keepControlConnectionAlive(room, player);
            return;
        }
        if (kind == "JOIN_ROOM")
        {
            if (!handleJoinRoom(sock, clientPk, req, room, player))
            {
                SDLNet_TCP_Close(sock);
                return;
            }
            keepControlConnectionAlive(room, player);
            return;
        }
        if (kind == "JOIN")
        {
            if (!handleLegacyJoin(sock, clientPk, req, room, player))
            {
                SDLNet_TCP_Close(sock);
                return;
            }
            keepControlConnectionAlive(room, player);
            return;
        }
        if (kind == "ROOM_HEARTBEAT")
        {
            handleRoomHeartbeatOrClose(sock, clientPk, req, false);
            SDLNet_TCP_Close(sock);
            return;
        }
        if (kind == "CLOSE_ROOM")
        {
            handleRoomHeartbeatOrClose(sock, clientPk, req, true);
            SDLNet_TCP_Close(sock);
            return;
        }

        sendError(sock, clientPk.data(), "unknown request kind");
        SDLNet_TCP_Close(sock);
    }

    bool verifyUdpRegister(const Json::Value& msg,
                           const std::shared_ptr<Player>& p)
    {
        if (!p)
            return false;
        Json::Value macData;
        macData["type"] = "UDP_REGISTER";
        macData["version"] = kProtocolVersion;
        macData["room"] = msg.get("room", "").asString();
        macData["player_id"] = msg.get("player_id", 0).asUInt();
        macData["nonce"] = msg.get("nonce", "").asString();
        const std::string toMac = compactJson(macData);

        std::vector<unsigned char> mac;
        if (!unb64(msg.get("mac", "").asString(), mac, crypto_auth_hmacsha256_BYTES))
            return false;

        unsigned char expected[crypto_auth_hmacsha256_BYTES];
        crypto_auth_hmacsha256(expected,
                               reinterpret_cast<const unsigned char*>(toMac.data()),
                               static_cast<unsigned long long>(toMac.size()),
                               p->udpToken.data());
        return sodium_memcmp(expected, mac.data(), sizeof(expected)) == 0;
    }

    void udpThread(uint16_t udpPort)
    {
        UDPsocket udp = SDLNet_UDP_Open(udpPort);
        if (!udp)
        {
            std::cerr << "Could not open rendezvous UDP port " << udpPort << "\n";
            g_stop.store(true);
            return;
        }
        UDPpacket* pkt = SDLNet_AllocPacket(1200);
        if (!pkt)
        {
            SDLNet_UDP_Close(udp);
            g_stop.store(true);
            return;
        }

        while (!g_stop.load())
        {
            while (SDLNet_UDP_Recv(udp, pkt) > 0)
            {
                std::string s(reinterpret_cast<char*>(pkt->data), static_cast<size_t>(pkt->len));
                Json::Value msg;
                if (!parseJson(s, msg))
                    continue;
                if (msg.get("type", "").asString() != "UDP_REGISTER")
                    continue;

                const std::string roomId = msg.get("room", "").asString();
                const uint32_t playerId = msg.get("player_id", 0).asUInt();
                std::shared_ptr<Room> room;
                std::shared_ptr<Player> player;
                {
                    std::lock_guard<std::mutex> lock(g_roomsMutex);
                    auto it = g_rooms.find(roomId);
                    if (it == g_rooms.end())
                        continue;
                    room = it->second;
                    if (room->closed)
                        continue;
                    for (auto& p : room->players)
                    {
                        if (p->playerId == playerId)
                        {
                            player = p;
                            break;
                        }
                    }
                    if (!verifyUdpRegister(msg, player))
                        continue;

                    player->udpAddress = pkt->address;
                    if (!player->udpRegistered)
                    {
                        std::cout << "UDP registered room=" << roomId
                                  << " player=" << player->playerName
                                  << " endpoint=" << ipToString(pkt->address)
                                  << ":" << portHostOrder(pkt->address) << "\n";
                    }
                    player->udpRegistered = true;
                    room->lastHeartbeatMs = nowMs();
                    maybeFinishRoomLocked(room);
                }
            }
            SDL_Delay(2);
        }

        SDLNet_FreePacket(pkt);
        SDLNet_UDP_Close(udp);
    }

    void cleanupThread()
    {
        while (!g_stop.load())
        {
            const uint64_t now = nowMs();
            {
                std::lock_guard<std::mutex> lock(g_roomsMutex);
                for (auto it = g_rooms.begin(); it != g_rooms.end(); )
                {
                    auto room = it->second;
                    if (kRoomTtlMs != 0 && !room->sessionKeyReady && !room->closed && now - room->lastHeartbeatMs > kRoomTtlMs)
                    {
                        std::cout << "Expiring stale room " << room->roomId << "\n";
                        it = g_rooms.erase(it);
                    }
                    else if (room->closed && now - room->lastHeartbeatMs > 30 * 1000)
                    {
                        it = g_rooms.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            SDL_Delay(5000);
        }
    }

    int parseIntArg(int argc, char** argv, const std::string& name, int def)
    {
        for (int i = 1; i + 1 < argc; ++i)
        {
            if (argv[i] == name)
                return std::atoi(argv[i + 1]);
        }
        return def;
    }

    std::string parseStringArg(int argc, char** argv, const std::string& name, const std::string& def)
    {
        for (int i = 1; i + 1 < argc; ++i)
        {
            if (argv[i] == name)
                return argv[i + 1];
        }
        return def;
    }
}

int main(int argc, char** argv)
{
    if (sodium_init() < 0)
    {
        std::cerr << "libsodium init failed\n";
        return 1;
    }

	// Force stdout/stderr to flush immediately when running under systemd.
	// Without this, journalctl -f may look empty until the process exits.
	std::cout.setf(std::ios::unitbuf);
	std::cerr.setf(std::ios::unitbuf);

    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--generate-keys" && i + 1 < argc)
        {
            return generateKeys(argv[i + 1]) ? 0 : 1;
        }
    }

    const int tcpPort = parseIntArg(argc, argv, "--tcp", 39000);
    const int udpPort = parseIntArg(argc, argv, "--udp", 39001);
    const std::string keysDir = parseStringArg(argc, argv, "--keys", "./rv_keys");

    if (!loadKeys(keysDir, g_keys))
    {
        std::cerr << "Could not load rendezvous keys from " << keysDir << "\n";
        std::cerr << "Run: " << argv[0] << " --generate-keys " << keysDir << "\n";
        return 1;
    }

    if (SDLNet_Init() == -1)
    {
        std::cerr << "SDLNet_Init failed\n";
        return 1;
    }

    IPaddress listenIp{};
    if (SDLNet_ResolveHost(&listenIp, nullptr, static_cast<Uint16>(tcpPort)) == -1)
    {
        std::cerr << "Could not resolve listen address\n";
        SDLNet_Quit();
        return 1;
    }

    TCPsocket server = SDLNet_TCP_Open(&listenIp);
    if (!server)
    {
        std::cerr << "Could not listen on TCP port " << tcpPort << "\n";
        SDLNet_Quit();
        return 1;
    }

    std::thread udp(udpThread, static_cast<uint16_t>(udpPort));
    std::thread clean(cleanupThread);

    std::cout << "Rendezvous server-list server listening TCP " << tcpPort
              << ", UDP " << udpPort << "\n";
    std::cout << "Press Ctrl+C to stop.\n";

    while (!g_stop.load())
    {
        TCPsocket client = SDLNet_TCP_Accept(server);
        if (client)
        {
            std::thread(clientThread, client).detach();
        }
        else
        {
            SDL_Delay(10);
        }
    }

    SDLNet_TCP_Close(server);
    if (udp.joinable()) udp.join();
    if (clean.joinable()) clean.join();
    SDLNet_Quit();
    return 0;
}
#endif // BUILD_RENDEZVOUS_SERVER
