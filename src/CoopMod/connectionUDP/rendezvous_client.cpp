#include "rendezvous_client.h"

#include <SDL_net.h>
#include <json/json.h>
#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <vector>

namespace OpenXcom
{
namespace
{
static const uint32_t kProtocolVersion = 2;
static const size_t kMaxFrame = 64 * 1024;
static const size_t kUdpRegisterNonceBytes = 24;
static const size_t kUdpRegisterTokenBytes = 32;

uint64_t nowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void setErr(std::string* error, const std::string& msg)
{
	if (error)
		*error = msg;
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

bool pollFrame(TCPsocket sock,
			   SDLNet_SocketSet set,
			   std::vector<unsigned char>& buffer,
			   std::string& out,
			   uint32_t timeoutMs)
{
	const int ready = SDLNet_CheckSockets(set, timeoutMs);
	if (ready > 0 && SDLNet_SocketReady(sock))
	{
		unsigned char tmp[4096];
		const int n = SDLNet_TCP_Recv(sock, tmp, sizeof(tmp));
		if (n <= 0)
			return false;
		buffer.insert(buffer.end(), tmp, tmp + n);
	}

	if (buffer.size() < 4)
		return false;

	const uint32_t len = readBE32(buffer.data());
	if (len == 0 || len > kMaxFrame)
	{
		buffer.clear();
		return false;
	}
	if (buffer.size() < 4u + len)
		return false;

	out.assign(reinterpret_cast<const char*>(buffer.data() + 4), len);
	buffer.erase(buffer.begin(), buffer.begin() + 4 + len);
	return true;
}

bool sealToPublicKey(const std::string& plain,
					 const unsigned char* publicKey,
					 std::string& sealed)
{
	sealed.resize(crypto_box_SEALBYTES + plain.size());
	return crypto_box_seal(reinterpret_cast<unsigned char*>(&sealed[0]),
						   reinterpret_cast<const unsigned char*>(plain.data()),
						   static_cast<unsigned long long>(plain.size()),
						   publicKey) == 0;
}

bool openSealed(const std::string& sealed,
				const unsigned char* publicKey,
				const unsigned char* secretKey,
				std::string& plain)
{
	if (sealed.size() < crypto_box_SEALBYTES)
		return false;
	plain.resize(sealed.size() - crypto_box_SEALBYTES);
	return crypto_box_seal_open(reinterpret_cast<unsigned char*>(&plain[0]),
								reinterpret_cast<const unsigned char*>(sealed.data()),
								static_cast<unsigned long long>(sealed.size()),
								publicKey,
								secretKey) == 0;
}

bool decryptAndVerifyServerMessage(const std::string& outerJson,
								   const unsigned char* clientPk,
								   const unsigned char* clientSk,
								   const unsigned char* serverSignPk,
								   Json::Value& inner)
{
	Json::Value outer;
	if (!parseJson(outerJson, outer))
		return false;
	if (outer.get("type", "").asString() != "SERVER_MSG")
		return false;

	std::vector<unsigned char> sealed;
	if (!unb64(outer.get("sealed", "").asString(), sealed))
		return false;

	std::string plain;
	if (!openSealed(std::string(reinterpret_cast<char*>(sealed.data()), sealed.size()),
					clientPk, clientSk, plain))
		return false;
	if (!parseJson(plain, inner))
		return false;

	std::vector<unsigned char> sig;
	if (!unb64(inner.get("server_sig", "").asString(), sig, crypto_sign_BYTES))
		return false;
	inner.removeMember("server_sig");
	const std::string signedPayload = compactJson(inner);
	return crypto_sign_verify_detached(sig.data(),
									   reinterpret_cast<const unsigned char*>(signedPayload.data()),
									   static_cast<unsigned long long>(signedPayload.size()),
									   serverSignPk) == 0;
}

bool sendEncryptedRequest(TCPsocket tcp,
						  const std::string& kind,
						  Json::Value payload,
						  const unsigned char* clientPk,
						  const unsigned char* serverBoxPk)
{
	payload["kind"] = kind;
	payload["version"] = kProtocolVersion;

	std::string sealed;
	if (!sealToPublicKey(compactJson(payload), serverBoxPk, sealed))
		return false;

	Json::Value outer;
	outer["type"] = "CLIENT_MSG";
	outer["version"] = kProtocolVersion;
	outer["client_box_pk"] = b64(clientPk, crypto_box_PUBLICKEYBYTES);
	outer["sealed"] = b64(reinterpret_cast<const unsigned char*>(sealed.data()), sealed.size());
	return sendFrame(tcp, compactJson(outer));
}

UDPsocket openClientUdp(uint16_t requestedPort, uint16_t& chosenPort)
{
	chosenPort = 0;

	// First try the requested local UDP port. This is the preferred path
	// because the rendezvous result must later start connectionUDP on the
	// same local port.
	if (requestedPort != 0)
	{
		UDPsocket s = SDLNet_UDP_Open(requestedPort);
		if (s)
		{
			chosenPort = requestedPort;
			return s;
		}

		// If the user disconnects and hosts again immediately, Windows may
		// still be releasing the old UDP socket. Wait briefly and retry the
		// same requested port before falling back to another port.
		SDL_Delay(250);

		s = SDLNet_UDP_Open(requestedPort);
		if (s)
		{
			chosenPort = requestedPort;
			return s;
		}
	}

	// SDL_net 1.2 does not expose a simple portable getter for an OS-selected
	// UDP port. Choose a high ephemeral port ourselves so connectionUDP can
	// bind the same one later. This also avoids failing host creation if the
	// requested port is still busy from the previous session.
	for (int i = 0; i < 128; ++i)
	{
		const uint16_t port = static_cast<uint16_t>(49152 + randombytes_uniform(16384));
		UDPsocket s = SDLNet_UDP_Open(port);
		if (s)
		{
			chosenPort = port;
			return s;
		}
	}

	// Last deterministic fallback. This makes the behavior less dependent
	// on random selection if many ephemeral ports are unavailable.
	for (uint16_t port = 40000; port < 40100; ++port)
	{
		UDPsocket s = SDLNet_UDP_Open(port);
		if (s)
		{
			chosenPort = port;
			return s;
		}
	}

	return nullptr;
}

bool sendUdpRegister(UDPsocket udp,
					 UDPpacket* pkt,
					 const IPaddress& serverUdp,
					 const std::string& roomId,
					 uint32_t playerId,
					 const std::vector<unsigned char>& token)
{
	if (token.size() != kUdpRegisterTokenBytes)
		return false;

	unsigned char nonce[kUdpRegisterNonceBytes];
	randombytes_buf(nonce, sizeof(nonce));

	Json::Value macData;
	macData["type"] = "UDP_REGISTER";
	macData["version"] = kProtocolVersion;
	macData["room"] = roomId;
	macData["player_id"] = Json::UInt(playerId);
	macData["nonce"] = b64(nonce, sizeof(nonce));
	const std::string toMac = compactJson(macData);

	unsigned char mac[crypto_auth_hmacsha256_BYTES];
	crypto_auth_hmacsha256(mac,
						   reinterpret_cast<const unsigned char*>(toMac.data()),
						   static_cast<unsigned long long>(toMac.size()),
						   token.data());

	macData["mac"] = b64(mac, sizeof(mac));
	const std::string out = compactJson(macData);
	if (out.size() > 1000)
		return false;

	pkt->address = serverUdp;
	pkt->len = static_cast<int>(out.size());
	std::memcpy(pkt->data, out.data(), out.size());
	return SDLNet_UDP_Send(udp, -1, pkt) > 0;
}

struct TcpCtx
{
	TCPsocket tcp = nullptr;
	SDLNet_SocketSet set = nullptr;
	std::vector<unsigned char> rx;
	unsigned char clientPk[crypto_box_PUBLICKEYBYTES];
	unsigned char clientSk[crypto_box_SECRETKEYBYTES];
};

bool openTcp(const RendezvousClient::CommonConfig& cfg, TcpCtx& ctx, std::string* error)
{
	IPaddress tcpAddr{};
	if (SDLNet_ResolveHost(&tcpAddr, cfg.serverHost.c_str(), cfg.serverTcpPort) == -1)
	{
		setErr(error, "could not resolve rendezvous TCP host");
		return false;
	}
	ctx.tcp = SDLNet_TCP_Open(&tcpAddr);
	if (!ctx.tcp)
	{
		setErr(error, "could not connect to rendezvous TCP server");
		return false;
	}
	ctx.set = SDLNet_AllocSocketSet(1);
	if (!ctx.set)
	{
		setErr(error, "could not allocate TCP socket set");
		return false;
	}
	SDLNet_TCP_AddSocket(ctx.set, ctx.tcp);
	crypto_box_keypair(ctx.clientPk, ctx.clientSk);
	return true;
}

void closeTcp(TcpCtx& ctx)
{
	if (ctx.set)
		SDLNet_FreeSocketSet(ctx.set);
	if (ctx.tcp)
		SDLNet_TCP_Close(ctx.tcp);
	ctx.set = nullptr;
	ctx.tcp = nullptr;
}

bool receiveSignedMessage(TcpCtx& ctx,
						  const RendezvousClient::CommonConfig& cfg,
						  Json::Value& msg,
						  uint32_t timeoutMs,
						  std::string* error)
{
	std::string frame;
	if (!pollFrame(ctx.tcp, ctx.set, ctx.rx, frame, timeoutMs))
		return false;
	if (!decryptAndVerifyServerMessage(frame,
									   ctx.clientPk,
									   ctx.clientSk,
									   cfg.keys.serverSignPublicKey.data(),
									   msg))
	{
		setErr(error, "invalid signed/encrypted rendezvous server message");
		return false;
	}
	return true;
}

bool initCommon()
{
	if (sodium_init() < 0)
		return false;
	if (SDLNet_Init() == -1)
		return false;
	return true;
}

void quitCommon()
{
	SDLNet_Quit();
}

bool validateCommon(const RendezvousClient::CommonConfig& cfg, std::string* error)
{
	if (cfg.serverHost.empty() || cfg.serverTcpPort == 0 || cfg.serverUdpPort == 0)
	{
		setErr(error, "missing rendezvous server config");
		return false;
	}
	return true;
}

bool isCancelled(const RendezvousClient::CommonConfig& cfg)
{
	return cfg.cancelRequested && cfg.cancelRequested();
}

bool waitForPeerReady(const RendezvousClient::CommonConfig& cfg,
					  TcpCtx& tcp,
					  UDPsocket udp,
					  UDPpacket* udpPkt,
					  const IPaddress& udpAddr,
					  const std::string& roomId,
					  uint16_t chosenLocalUdpPort,
					  RendezvousClient::Result& out,
					  std::string* error)
{
	uint64_t sessionId = 0;
	uint32_t playerId = 0;
	std::vector<unsigned char> udpToken;
	uint64_t nextUdpRegisterMs = 0;
	const bool infiniteWait = (cfg.timeoutMs == 0);
	const uint64_t deadline = infiniteWait ? 0 : (nowMs() + cfg.timeoutMs);

	while (infiniteWait || nowMs() < deadline)
	{
		if (isCancelled(cfg))
		{
			setErr(error, "rendezvous cancelled");
			return false;
		}

		if (!udpToken.empty() && playerId != 0 && nowMs() >= nextUdpRegisterMs)
		{
			sendUdpRegister(udp, udpPkt, udpAddr, roomId, playerId, udpToken);
			nextUdpRegisterMs = nowMs() + 300;
		}

		Json::Value msg;
		std::string localErr;
		if (!receiveSignedMessage(tcp, cfg, msg, 300, &localErr))
			continue;

		const std::string kind = msg.get("kind", "").asString();
		if (kind == "ERROR")
		{
			setErr(error, msg.get("message", "rendezvous server error").asString());
			return false;
		}
		if (kind == "CREATE_ROOM_OK" || kind == "JOIN_ROOM_OK" || kind == "JOIN_OK")
		{
			out.roomId = msg.get("room_id", roomId).asString();
			out.hostToken = msg.get("host_token", "").asString();
			sessionId = static_cast<uint64_t>(std::strtoull(msg.get("session_id", "0").asCString(), nullptr, 10));
			playerId = msg.get("player_id", 0).asUInt();
			if (!unb64(msg.get("udp_token", "").asString(), udpToken, kUdpRegisterTokenBytes))
			{
				setErr(error, "JOIN/CREATE response had invalid UDP token");
				return false;
			}
			if (cfg.log)
				cfg.log(kind + ", waiting for peer");
			continue;
		}
		if (kind == "WAITING")
		{
			continue;
		}
		if (kind == "PEER_READY")
		{
			std::vector<unsigned char> sessionKey;
			if (!unb64(msg.get("session_key", "").asString(), sessionKey, connectionUDP::kSessionKeyBytes))
			{
				setErr(error, "PEER_READY had invalid session key");
				return false;
			}

			out.remoteHost = msg.get("remote_host", "").asString();
			out.remotePort = static_cast<uint16_t>(msg.get("remote_port", 0).asUInt());
			out.localPort = chosenLocalUdpPort;
			out.sessionId = sessionId;
			out.localPlayerId = playerId;
			out.remotePlayerId = msg.get("remote_player_id", 0).asUInt();
			out.peerPlayerName = msg.get("peer_player_name", "").asString();
			std::copy(sessionKey.begin(), sessionKey.end(), out.sessionKey.begin());
			return !out.remoteHost.empty() && out.remotePort != 0 && out.sessionId != 0 &&
				   out.localPlayerId != 0 && out.remotePlayerId != 0;
		}
	}

	setErr(error, "rendezvous timed out before peer was ready");
	return false;
}

bool createOrJoinAndWait(const RendezvousClient::CommonConfig& common,
						 const std::string& kind,
						 Json::Value payload,
						 uint16_t localUdpPort,
						 const std::string& roomIdForUdp,
						 RendezvousClient::Result& out,
						 std::string* error)
{
	if (!validateCommon(common, error))
		return false;
	if (!initCommon())
	{
		setErr(error, "libsodium or SDLNet init failed");
		return false;
	}

	TcpCtx tcp;
	UDPsocket udp = nullptr;
	UDPpacket* udpPkt = nullptr;
	auto cleanup = [&]()
	{
		closeTcp(tcp);
		if (udpPkt)
			SDLNet_FreePacket(udpPkt);
		if (udp)
			SDLNet_UDP_Close(udp);
		quitCommon();
	};

	if (!openTcp(common, tcp, error))
	{
		cleanup();
		return false;
	}

	IPaddress udpAddr{};
	if (SDLNet_ResolveHost(&udpAddr, common.serverHost.c_str(), common.serverUdpPort) == -1)
	{
		cleanup();
		setErr(error, "could not resolve rendezvous UDP host");
		return false;
	}

	uint16_t chosenLocalUdpPort = 0;
	udp = openClientUdp(localUdpPort, chosenLocalUdpPort);
	if (!udp || chosenLocalUdpPort == 0)
	{
		cleanup();
		setErr(error, "could not open local UDP socket for rendezvous registration");
		return false;
	}
	udpPkt = SDLNet_AllocPacket(1200);
	if (!udpPkt)
	{
		cleanup();
		setErr(error, "could not allocate UDP packet");
		return false;
	}

	payload["udp_local_port"] = Json::UInt(chosenLocalUdpPort);
	if (!sendEncryptedRequest(tcp.tcp, kind, payload, tcp.clientPk, common.keys.serverBoxPublicKey.data()))
	{
		cleanup();
		setErr(error, "failed to send rendezvous request");
		return false;
	}

	bool ok = waitForPeerReady(common, tcp, udp, udpPkt, udpAddr,
							   roomIdForUdp, chosenLocalUdpPort, out, error);
	cleanup();
	return ok;
}
}

bool RendezvousClient::listRooms(const ListConfig& cfg,
								 std::vector<RoomInfo>& outRooms,
								 std::string* error)
{
	outRooms.clear();
	if (!validateCommon(cfg, error))
		return false;
	if (!initCommon())
	{
		setErr(error, "libsodium or SDLNet init failed");
		return false;
	}

	TcpCtx tcp;
	auto cleanup = [&]()
	{ closeTcp(tcp); quitCommon(); };
	if (!openTcp(cfg, tcp, error))
	{
		cleanup();
		return false;
	}

	Json::Value req;
	req["game_version"] = cfg.gameVersion;
	req["mod_hash"] = cfg.modHash;
	req["compatible_only"] = cfg.compatibleOnly;

	if (!sendEncryptedRequest(tcp.tcp, "LIST_ROOMS", req, tcp.clientPk, cfg.keys.serverBoxPublicKey.data()))
	{
		cleanup();
		setErr(error, "failed to send LIST_ROOMS");
		return false;
	}

	const bool infiniteWait = (cfg.timeoutMs == 0);
	const uint64_t deadline = infiniteWait ? 0 : (nowMs() + cfg.timeoutMs);
	while (infiniteWait || nowMs() < deadline)
	{
		if (isCancelled(cfg))
		{
			cleanup();
			setErr(error, "rendezvous cancelled");
			return false;
		}

		Json::Value msg;
		std::string localErr;
		if (!receiveSignedMessage(tcp, cfg, msg, 300, &localErr))
			continue;

		const std::string kind = msg.get("kind", "").asString();
		if (kind == "ERROR")
		{
			cleanup();
			setErr(error, msg.get("message", "rendezvous server error").asString());
			return false;
		}
		if (kind == "ROOM_LIST")
		{
			const Json::Value& arr = msg["rooms"];
			for (Json::ArrayIndex i = 0; i < arr.size(); ++i)
			{
				RoomInfo r;
				r.roomId = arr[i].get("room_id", "").asString();
				r.name = arr[i].get("name", "").asString();
				r.hostName = arr[i].get("host_name", "").asString();
				r.region = arr[i].get("region", "").asString();
				r.players = arr[i].get("players", 0).asUInt();
				r.maxPlayers = arr[i].get("max_players", 0).asUInt();
				r.locked = arr[i].get("locked", false).asBool();
				r.passwordRequired = arr[i].get("password_required", false).asBool();
				r.isCampaign = arr[i].get("is_campaign", false).asBool();
				r.gameVersion = arr[i].get("game_version", "").asString();
				r.modHash = arr[i].get("mod_hash", "").asString();
				if (!r.roomId.empty())
					outRooms.push_back(r);
			}
			cleanup();
			return true;
		}
	}

	cleanup();
	setErr(error, "LIST_ROOMS timed out");
	return false;
}

bool RendezvousClient::createRoomAndWait(const CreateRoomConfig& cfg,
										 Result& out,
										 std::string* error)
{
	if (cfg.playerName.empty())
	{
		setErr(error, "player name missing");
		return false;
	}

	Json::Value req;
	req["room_name"] = cfg.roomName.empty() ? "Game" : cfg.roomName;
	req["player_name"] = cfg.playerName;
	req["region"] = cfg.region;
	req["password"] = cfg.password;
	req["listed"] = cfg.listed;
	req["password_required"] = cfg.passwordRequired || !cfg.password.empty();
	req["is_campaign"] = cfg.isCampaign;
	req["game_version"] = cfg.gameVersion;
	req["mod_hash"] = cfg.modHash;
	req["desired_players"] = Json::UInt(cfg.desiredPlayers);

	// Room id is generated by the server, so UDP_REGISTER uses out.roomId after CREATE_ROOM_OK.
	// The wait loop starts with an empty room id but updates out.roomId when CREATE_ROOM_OK arrives.
	// UDP_REGISTER needs room id; therefore use a small wrapper strategy: after CREATE_ROOM_OK,
	// waitForPeerReady uses out.roomId if the original string is empty.
	// To keep code simple here, server also echoes room_id and waitForPeerReady updates out.roomId before sending UDP.
	// However sendUdpRegister receives roomId parameter, so we need create-specific inline loop below.

	if (!validateCommon(cfg, error))
		return false;
	if (!initCommon())
	{
		setErr(error, "libsodium or SDLNet init failed");
		return false;
	}

	TcpCtx tcp;
	UDPsocket udp = nullptr;
	UDPpacket* udpPkt = nullptr;
	auto cleanup = [&]()
	{
		closeTcp(tcp);
		if (udpPkt)
			SDLNet_FreePacket(udpPkt);
		if (udp)
			SDLNet_UDP_Close(udp);
		quitCommon();
	};

	if (!openTcp(cfg, tcp, error))
	{
		cleanup();
		return false;
	}

	IPaddress udpAddr{};
	if (SDLNet_ResolveHost(&udpAddr, cfg.serverHost.c_str(), cfg.serverUdpPort) == -1)
	{
		cleanup();
		setErr(error, "could not resolve rendezvous UDP host");
		return false;
	}
	uint16_t chosenLocalUdpPort = 0;
	udp = openClientUdp(cfg.localUdpPort, chosenLocalUdpPort);
	if (!udp || chosenLocalUdpPort == 0)
	{
		cleanup();
		setErr(error, "could not open local UDP socket for rendezvous registration");
		return false;
	}
	udpPkt = SDLNet_AllocPacket(1200);
	if (!udpPkt)
	{
		cleanup();
		setErr(error, "could not allocate UDP packet");
		return false;
	}
	req["udp_local_port"] = Json::UInt(chosenLocalUdpPort);

	if (!sendEncryptedRequest(tcp.tcp, "CREATE_ROOM", req, tcp.clientPk, cfg.keys.serverBoxPublicKey.data()))
	{
		cleanup();
		setErr(error, "failed to send CREATE_ROOM");
		return false;
	}

	uint64_t sessionId = 0;
	uint32_t playerId = 0;
	std::vector<unsigned char> udpToken;
	uint64_t nextUdpRegisterMs = 0;
	const bool infiniteWait = (cfg.timeoutMs == 0);
	const uint64_t deadline = infiniteWait ? 0 : (nowMs() + cfg.timeoutMs);

	while (infiniteWait || nowMs() < deadline)
	{
		if (isCancelled(cfg))
		{
			cleanup();
			setErr(error, "rendezvous cancelled");
			return false;
		}

		if (!udpToken.empty() && playerId != 0 && !out.roomId.empty() && nowMs() >= nextUdpRegisterMs)
		{
			sendUdpRegister(udp, udpPkt, udpAddr, out.roomId, playerId, udpToken);
			nextUdpRegisterMs = nowMs() + 300;
		}

		Json::Value msg;
		if (!receiveSignedMessage(tcp, cfg, msg, 300, error))
			continue;
		const std::string kind = msg.get("kind", "").asString();
		if (kind == "ERROR")
		{
			cleanup();
			setErr(error, msg.get("message", "rendezvous server error").asString());
			return false;
		}
		if (kind == "CREATE_ROOM_OK")
		{
			out.roomId = msg.get("room_id", "").asString();
			out.hostToken = msg.get("host_token", "").asString();
			sessionId = static_cast<uint64_t>(std::strtoull(msg.get("session_id", "0").asCString(), nullptr, 10));
			playerId = msg.get("player_id", 0).asUInt();
			if (!unb64(msg.get("udp_token", "").asString(), udpToken, kUdpRegisterTokenBytes))
			{
				cleanup();
				setErr(error, "CREATE_ROOM_OK had invalid UDP token");
				return false;
			}
			if (cfg.onRoomCreated)
				cfg.onRoomCreated(out.roomId, out.hostToken, sessionId, playerId, chosenLocalUdpPort);

			if (cfg.log)
				cfg.log("CREATE_ROOM_OK room=" + out.roomId + ", waiting for peer");
			continue;
		}
		if (kind == "WAITING")
			continue;
		if (kind == "PEER_READY")
		{
			std::vector<unsigned char> sessionKey;
			if (!unb64(msg.get("session_key", "").asString(), sessionKey, connectionUDP::kSessionKeyBytes))
			{
				cleanup();
				setErr(error, "PEER_READY had invalid session key");
				return false;
			}
			out.remoteHost = msg.get("remote_host", "").asString();
			out.remotePort = static_cast<uint16_t>(msg.get("remote_port", 0).asUInt());
			out.localPort = chosenLocalUdpPort;
			out.sessionId = sessionId;
			out.localPlayerId = playerId;
			out.remotePlayerId = msg.get("remote_player_id", 0).asUInt();
			out.peerPlayerName = msg.get("peer_player_name", "").asString();
			std::copy(sessionKey.begin(), sessionKey.end(), out.sessionKey.begin());
			cleanup();
			return !out.remoteHost.empty() && out.remotePort != 0 && out.sessionId != 0 && out.localPlayerId != 0;
		}
	}

	cleanup();
	setErr(error, "CREATE_ROOM timed out before peer was ready");
	return false;
}

bool RendezvousClient::joinRoomAndWait(const JoinRoomConfig& cfg,
									   Result& out,
									   std::string* error)
{
	if (cfg.roomId.empty() || cfg.playerName.empty())
	{
		setErr(error, "room id or player name missing");
		return false;
	}
	Json::Value req;
	req["room_id"] = cfg.roomId;
	req["player_name"] = cfg.playerName;
	req["password"] = cfg.password;
	req["game_version"] = cfg.gameVersion;
	req["mod_hash"] = cfg.modHash;

	bool ok = createOrJoinAndWait(cfg, "JOIN_ROOM", req, cfg.localUdpPort, cfg.roomId, out, error);
	if (ok && out.roomId.empty())
		out.roomId = cfg.roomId;
	return ok;
}

bool RendezvousClient::closeRoom(const RoomControlConfig& cfg, std::string* error)
{
	if (cfg.roomId.empty() || cfg.hostToken.empty())
	{
		setErr(error, "room id or host token missing");
		return false;
	}
	if (!validateCommon(cfg, error))
		return false;
	if (!initCommon())
	{
		setErr(error, "libsodium or SDLNet init failed");
		return false;
	}

	TcpCtx tcp;
	auto cleanup = [&]()
	{ closeTcp(tcp); quitCommon(); };
	if (!openTcp(cfg, tcp, error))
	{
		cleanup();
		return false;
	}

	Json::Value req;
	req["room_id"] = cfg.roomId;
	req["host_token"] = cfg.hostToken;

	if (!sendEncryptedRequest(tcp.tcp, "CLOSE_ROOM", req, tcp.clientPk, cfg.keys.serverBoxPublicKey.data()))
	{
		cleanup();
		setErr(error, "failed to send CLOSE_ROOM");
		return false;
	}

	const bool infiniteWait = (cfg.timeoutMs == 0);
	const uint64_t deadline = infiniteWait ? 0 : (nowMs() + cfg.timeoutMs);
	while (infiniteWait || nowMs() < deadline)
	{
		if (isCancelled(cfg))
		{
			cleanup();
			setErr(error, "rendezvous cancelled");
			return false;
		}

		Json::Value msg;
		std::string localErr;
		if (!receiveSignedMessage(tcp, cfg, msg, 300, &localErr))
			continue;

		const std::string kind = msg.get("kind", "").asString();
		if (kind == "ERROR")
		{
			cleanup();
			setErr(error, msg.get("message", "rendezvous server error").asString());
			return false;
		}
		if (kind == "CLOSE_ROOM_OK")
		{
			cleanup();
			return true;
		}
	}

	cleanup();
	setErr(error, "CLOSE_ROOM timed out");
	return false;
}

bool RendezvousClient::perform(const Config& cfg,
							   Result& out,
							   std::string* error)
{
	if (cfg.roomId.empty() || cfg.roomPassword.empty() || cfg.playerName.empty())
	{
		setErr(error, "missing rendezvous config");
		return false;
	}
	Json::Value req;
	req["room"] = cfg.roomId;
	req["password"] = cfg.roomPassword;
	req["player_name"] = cfg.playerName;
	req["game_version"] = cfg.gameVersion;
	req["mod_hash"] = cfg.modHash;
	req["desired_players"] = Json::UInt(cfg.desiredPlayers);

	bool ok = createOrJoinAndWait(cfg, "JOIN", req, cfg.localUdpPort, cfg.roomId, out, error);
	if (ok && out.roomId.empty())
		out.roomId = cfg.roomId;
	return ok;
}

} 
