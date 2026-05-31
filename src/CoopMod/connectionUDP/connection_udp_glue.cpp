/*
 * connectionUDP <-> connectionTCP integration glue.
 *
 * This file is intentionally written to use the existing connectionTCP state
 * instead of maintaining a second set of multiplayer variables.
 *
 * Integration model:
 *  - outgoing JSON still goes through the existing g_txQ queue
 *  - incoming ordered JSON still goes into the existing g_rxQ queue
 *  - connectionTCP::updateCoopTask() already drains g_rxQ and calls
 *    connectionTCP::onTCPMessage(stateString, obj), so UDP and TCP share the
 *    same packet execution path
 *  - static connectionTCP lobby/session variables are updated directly here
 *
 * If you move g_txQ/g_rxQ/onConnect/server_owner into connectionTCP as public
 * static members later, replace the extern globals below with those members.
 */

#include "connection_udp_glue.h"
#include "../connectionTCP.h"
#include "connection_rendezvous_glue.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>

namespace OpenXcom
{

// These are currently globals in the provided connectionTCP.cpp. They are kept
// here to avoid changing all old TCP code at once. New connectionTCP static
// state, such as connectionTCP::isCoopSessionLocked, is used directly below.
extern SPSCQueue<kNetworkQueueSize> g_txQ;
extern SPSCQueue<kNetworkQueueSize> g_rxQ;
extern int onConnect;
extern bool server_owner;
extern bool onTcpHost;
extern bool clearPackets;
extern bool coopSession;
extern std::string current_ping;

// One UDP transport for the current 2-player session.
// If preferred, declare this as public static in connectionTCP.h:
//   static std::unique_ptr<connectionUDP> connectionUDP;
// and define it in connectionTCP.cpp instead.
static std::unique_ptr<connectionUDP> s_connectionUDP;
static bool s_udpEnabled = false;
static std::thread s_udpPingThread;
static std::atomic<bool> s_udpPingStop(false);

static uint64_t nowMsForGlue()
{
	using namespace std::chrono;
	return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

static std::string jsonToCompactString(const Json::Value& v)
{
	Json::StreamWriterBuilder wb;
	wb["indentation"] = "";
	return Json::writeString(wb, v);
}

static uint64_t readDirectSessionId(const unsigned char* p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i)
		v = (v << 8) | static_cast<uint64_t>(p[i]);
	if (v == 0)
		v = 1;
	return v;
}

static bool deriveDirectLanSession(const std::string& password,
								   uint64_t& sessionId,
								   std::array<unsigned char, connectionUDP::kSessionKeyBytes>& sessionKey)
{
	if (sodium_init() < 0)
		return false;

	std::string material = "OpenXcom connectionUDP direct LAN v1\n";
	material += password;

	unsigned char hash[crypto_generichash_BYTES];
	if (crypto_generichash(hash,
						   sizeof(hash),
						   reinterpret_cast<const unsigned char*>(material.data()),
						   static_cast<unsigned long long>(material.size()),
						   nullptr,
						   0) != 0)
	{
		return false;
	}

	sessionId = readDirectSessionId(hash);
	std::memcpy(sessionKey.data(), hash, sessionKey.size());
	sodium_memzero(hash, sizeof(hash));
	return true;
}

static bool parseJsonForUdpControl(const std::string& msg, Json::Value& out)
{
	Json::CharReaderBuilder rb;
	std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
	std::string errs;
	const char* begin = msg.data();
	const char* end = begin + msg.size();
	return reader->parse(begin, end, &out, &errs);
}

static void sendUdpPingNow()
{
	Json::Value ping;
	ping["type"] = "PING";
	ping["ts"] = Json::UInt64(nowMsForGlue());
	enqueueTx(jsonToCompactString(ping));
}

static void sendUdpPong(uint64_t ts)
{
	Json::Value pong;
	pong["type"] = "PONG";
	pong["ts"] = Json::UInt64(ts);
	enqueueTx(jsonToCompactString(pong));
}

static bool handleUdpInternalPingPong(const std::string& msg)
{
	// Avoid parsing every gameplay JSON message just to check for legacy
	// ping/pong packets. Most game packets use "state", not "type".
	if (msg.find("\"type\":\"PING\"") == std::string::npos &&
		msg.find("\"type\":\"PONG\"") == std::string::npos)
	{
		return false;
	}

	Json::Value obj;
	if (!parseJsonForUdpControl(msg, obj))
		return false;

	const std::string type = obj.get("type", "").asString();
	if (type == "PING")
	{
		sendUdpPong(obj.get("ts", Json::UInt64(0)).asUInt64());
		return true;
	}

	if (type == "PONG")
	{
		const uint64_t sent = obj.get("ts", Json::UInt64(0)).asUInt64();
		if (sent != 0)
		{
			const uint64_t rtt = nowMsForGlue() - sent;
			current_ping = std::to_string(static_cast<unsigned long long>(rtt));
		}
		return true;
	}

	return false;
}

static void startUdpPingThread()
{
	s_udpPingStop.store(false);
	if (s_udpPingThread.joinable())
		s_udpPingThread.join();

	s_udpPingThread = std::thread([]()
								  {
        while (!s_udpPingStop.load())
        {
            if (s_udpEnabled && s_connectionUDP && !s_connectionUDP->isRunning())
            {
                // The UDP worker stopped by itself, usually because the peer
                // sent F_CLOSE or the socket failed. Expose the same state the
                // old TCP path used for a remote disconnect.
                s_udpEnabled = false;
                DebugLog("connectionUDP: peer disconnected / UDP worker stopped\n");
                handleUdpRemotePeerLost();
                return;
            }

            // Do not send JSON PING/PONG through the reliable ordered gameplay
            // queue. A dropped ping packet can block later gameplay messages
            // behind reliable ordering and create visible latency spikes.
            if (s_udpEnabled && s_connectionUDP && s_connectionUDP->isRunning() && s_connectionUDP->isPeerReady())
            {
                const uint64_t rtt = s_connectionUDP->currentRttMs();
                if (rtt != 0)
                    current_ping = std::to_string(static_cast<unsigned long long>(rtt));
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        } });
}

static void stopUdpPingThread()
{
	s_udpPingStop.store(true);
	if (s_udpPingThread.joinable())
		s_udpPingThread.join();
}

static void enqueueInitServerPacket(const std::string& playerName)
{
	Json::Value hello;
	hello["state"] = "INIT_SERVER";
	hello["playername"] = playerName.empty() ? "Player" : playerName;
	enqueueTx(jsonToCompactString(hello));
}

bool isConnectionUDPActive()
{
	return s_udpEnabled && s_connectionUDP && s_connectionUDP->isRunning();
}

// Use this when replacing sendTCPPacketStaticData(...) in connectionTCP.cpp.
// It pushes into the same queue as TCP, so loopData()/game code does not need
// to know whether the active transport is TCP or UDP.
void sendUDPPacketStaticData(std::string data)
{
	// Keep one enqueue path for TCP and UDP. Normal gameplay code may keep
	// calling sendTCPPacketStaticData(...); this wrapper exists for new UDP-only
	// call sites and testing.
	enqueueTx(std::move(data));
}

// Start after rendezvous has returned peer endpoint, session id and key.
// 2-player mode: playerId 1 = room creator/host, playerId 2 = joining client.
bool startUdpPeer(const std::string& remoteHost,
				  uint16_t remotePort,
				  uint16_t localPort,
				  uint64_t sessionId,
				  const std::array<unsigned char, connectionUDP::kSessionKeyBytes>& sessionKey,
				  bool isHost,
				  const std::string& playerName,
				  bool sendInitServerWhenClient)
{
	DebugLog(("connectionUDP: startUdpPeer remote=" +
			  remoteHost + ":" + std::to_string(remotePort) +
			  " localPort=" + std::to_string(localPort) +
			  " sessionId=" + std::to_string(sessionId) +
			  " isHost=" + std::to_string(isHost ? 1 : 0) + "\n")
				 .c_str());

	stopUdpPingThread();

	if (s_connectionUDP)
		s_connectionUDP->stop();

	s_connectionUDP.reset(new connectionUDP());

	connectionUDP::Config cfg;
	cfg.localPort = localPort;
	cfg.remoteHost = remoteHost;
	cfg.remotePort = remotePort;
	cfg.sessionId = sessionId;
	cfg.localPlayerId = isHost ? 1u : 2u;
	cfg.remotePlayerId = isHost ? 2u : 1u;
	cfg.sessionKey = sessionKey;
	cfg.enablePortGuessing = true;
	cfg.portGuessRadius = 32;

	// TX: read exactly the same queue that TCP used.
	cfg.popTx = [](std::string& out) -> bool
	{
		return g_txQ.pop(out);
	};

	// RX: write exactly the same queue that connectionTCP::updateCoopTask()
	// already reads before calling connectionTCP::onTCPMessage(...).
	cfg.pushRx = [](std::string&& msg) -> bool
	{
		if (handleUdpInternalPingPong(msg))
			return true;

		return g_rxQ.push(std::move(msg));
	};

	cfg.log = [](const std::string& s)
	{
		DebugLog(("connectionUDP: " + s + "\n").c_str());
	};

	if (!s_connectionUDP->start(cfg))
	{
		DebugLog("connectionUDP: start failed\n");

		s_connectionUDP.reset();
		s_udpEnabled = false;
		onConnect = -3;
		return false;
	}

	DebugLog("connectionUDP: UDP peer object started\n");

	// Reuse existing connectionTCP/global state. Do not create duplicate UDP
	// lobby variables.
	s_udpEnabled = true;
	coopSession = true;
	server_owner = isHost;
	onTcpHost = isHost;
	onConnect = 1;

	connectionTCP::isCoopSessionLocked = false;
	connectionTCP::isPlayerReady = false;
	connectionTCP::isPlayersReady = false;
	connectionTCP::LobbyFileStatus = -1;
	connectionTCP::lobby_timer = -1;

	startUdpPingThread();

	// Match the old TCP client behavior. In the TCP path, the joining client
	// sends INIT_SERVER once after connecting. With UDP, wait until the peer
	// authentication/hole punching has actually completed.
	if (!isHost && sendInitServerWhenClient)
	{
		DebugLog("connectionUDP: waiting before INIT_SERVER until peer ready\n");

		std::thread([playerName]()
					{
            for (int i = 0; i < 300; ++i) // 30 seconds max
            {
                if (!isConnectionUDPActive())
                {
                    DebugLog("connectionUDP: INIT_SERVER cancelled, UDP inactive\n");
                    return;
                }

                if (s_connectionUDP && s_connectionUDP->isPeerReady())
                {
                    DebugLog("connectionUDP: sending INIT_SERVER now\n");
                    enqueueInitServerPacket(playerName);
                    return;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            DebugLog("connectionUDP: INIT_SERVER timeout waiting for peer ready\n"); })
			.detach();
	}

	return true;
}

bool startDirectLanHost(uint16_t localUdpPort,
						const std::string& playerName,
						const std::string& password)
{
	if (localUdpPort == 0)
		localUdpPort = 3000;

	uint64_t sessionId = 0;
	std::array<unsigned char, connectionUDP::kSessionKeyBytes> sessionKey{};
	if (!deriveDirectLanSession(password, sessionId, sessionKey))
	{
		DebugLog("connectionUDP direct LAN host: failed to derive session key\n");
		onConnect = -3;
		return false;
	}

	DebugLog(("connectionUDP direct LAN host: listening localPort=" +
			  std::to_string(localUdpPort) +
			  " password=" + (password.empty() ? std::string("empty") : std::string("set")) +
			  "\n")
				 .c_str());

	// Host does not know the client LAN endpoint yet. connectionUDP accepts the
	// first authenticated packet that matches sessionId/sessionKey and player ids.
	const bool ok = startUdpPeer(std::string(),
								 0,
								 localUdpPort,
								 sessionId,
								 sessionKey,
								 true,
								 playerName,
								 false);
	if (!ok)
		onConnect = -3;
	return ok;
}

void lockUdpSessionWhenBothReady()
{
	if (s_connectionUDP)
		s_connectionUDP->lockSessionToCurrentPeer();

	// Match existing TCP lobby state, so old UI/game logic sees the same state.
	connectionTCP::isCoopSessionLocked = true;
	connectionTCP::isPlayersReady = true;
}

void stopUdpPeer()
{
	stopUdpPingThread();

	if (s_connectionUDP)
	{
		s_connectionUDP->stop();
		s_connectionUDP.reset();
	}

	// Only stop the UDP transport. Do not change onConnect, coopSession,
	// server_owner or onTcpHost here. The old connectionTCP::disconnectTCP()
	// logic uses onConnect to choose between full close (-1) and remote
	// disconnect / keep-host-waiting (-2).
	s_udpEnabled = false;
}

void clearAllReceivedUDPPackets()
{
	clearPackets = false;

	std::string drop;
	while (g_rxQ.pop(drop))
	{
	}

	if (s_connectionUDP)
		s_connectionUDP->clearQueues();
}

// Optional compatibility wrappers for older call sites from the previous draft.
bool startCoopUdpPeer(const std::string& remoteHost,
					  uint16_t remotePort,
					  uint16_t localPort,
					  uint64_t sessionId,
					  const std::array<unsigned char, connectionUDP::kSessionKeyBytes>& sessionKey,
					  bool isHost,
					  const std::string& playerName,
					  bool sendInitServerWhenClient)
{
	return startUdpPeer(remoteHost, remotePort, localPort, sessionId, sessionKey, isHost, playerName, sendInitServerWhenClient);
}

void lockCoopUdpSessionWhenBothReady()
{
	lockUdpSessionWhenBothReady();
}

void stopCoopUdpPeer()
{
	stopUdpPeer();
}

} 
