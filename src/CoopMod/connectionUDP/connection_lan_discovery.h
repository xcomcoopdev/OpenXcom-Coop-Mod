#pragma once

#include "rendezvous_client.h"

#include <cstdint>
#include <string>
#include <vector>

namespace OpenXcom
{

// Fixed LAN discovery UDP port. This is only for finding rooms on the local
// network. The actual game UDP port can be different and is carried in
// RoomInfo::lanPort.
static const uint16_t kLanDiscoveryPort = 39002;

// Starts or updates the host-side LAN discovery responder for the currently
// listed rendezvous room. This does not create a second UDP game session; it
// only advertises the existing rendezvous room on the local network.
void startLanDiscoveryHost(const RendezvousClient::RoomInfo& room);

// Stops the host-side LAN discovery responder.
void stopLanDiscoveryHost();

// Sends a LAN broadcast to kLanDiscoveryPort and merges discovered LAN rooms
// into ioRooms. If a LAN room has the same roomId as an existing internet
// rendezvous room, that existing row is marked as LAN and gets lanHost/lanPort.
void refreshLanServerList(std::vector<RendezvousClient::RoomInfo>& ioRooms,
                          const std::string& gameVersion,
                          const std::string& modHash,
                          bool compatibleOnly,
                          uint32_t timeoutMs = 500);

// Sends a unicast LAN discovery query to a specific host IP on kLanDiscoveryPort.
// This is used by the Direct Connect menu when the user knows the host LAN IP
// but not the rendezvous roomId. Returns true if one valid LAN room response
// was received from that address.
bool findLanRoomByAddress(const std::string& hostLanIp,
                          RendezvousClient::RoomInfo& outRoom,
                          const std::string& gameVersion,
                          const std::string& modHash,
                          bool compatibleOnly,
                          uint32_t timeoutMs = 500);

}
