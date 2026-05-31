#pragma once

#include "connectionUDP.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace OpenXcom
{

bool isConnectionUDPActive();

// Optional compatibility function. Normal gameplay code can keep using
// sendTCPPacketStaticData(...), because connectionUDP reads the same g_txQ queue.
void sendUDPPacketStaticData(std::string data);

bool startUdpPeer(const std::string& remoteHost,
                  uint16_t remotePort,
                  uint16_t localPort,
                  uint64_t sessionId,
                  const std::array<unsigned char, connectionUDP::kSessionKeyBytes>& sessionKey,
                  bool isHost,
                  const std::string& playerName = std::string(),
                  bool sendInitServerWhenClient = true);

using DirectLanBoolCallback = std::function<void(bool ok)>;

// Direct LAN host helper kept for compatibility with older drafts.
// The normal host path is hostListedViaRendezvousAsync(...), which creates one
// shared Internet/LAN-discovery room.
// password may be empty; it deterministically derives the UDP sessionId/sessionKey.
bool startDirectLanHost(uint16_t localUdpPort,
                        const std::string& playerName,
                        const std::string& password);

void lockUdpSessionWhenBothReady();
void stopUdpPeer();
void clearAllReceivedUDPPackets();

// Backwards-compatible names from earlier drafts.
bool startCoopUdpPeer(const std::string& remoteHost,
                      uint16_t remotePort,
                      uint16_t localPort,
                      uint64_t sessionId,
                      const std::array<unsigned char, connectionUDP::kSessionKeyBytes>& sessionKey,
                      bool isHost,
                      const std::string& playerName = std::string(),
                      bool sendInitServerWhenClient = true);

void lockCoopUdpSessionWhenBothReady();
void stopCoopUdpPeer();

} 
