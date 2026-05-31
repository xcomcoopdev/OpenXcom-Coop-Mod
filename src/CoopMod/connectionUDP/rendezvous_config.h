#pragma once

/*
 * Built-in rendezvous configuration for the OpenXcom UDP transport.
 *
 * Keep these values in one place so UI/menu code does not need to pass the
 * rendezvous IP, ports, game version or public key paths every time it calls
 * host/list/join helpers.
 *
 * SECURITY:
 *  - Only public keys belong here.
 *  - Never put server_box_secret.key or server_sign_secret.key in the client.
 */

#include "rendezvous_client.h"

#include <cstdint>
#include <string>

namespace OpenXcom
{
extern const char* kRendezvousHost;
extern const uint16_t kRendezvousTcpPort;
extern const uint16_t kRendezvousUdpPort;
extern const char* kRendezvousGameVersion;

struct BuiltInRendezvousConfig
{
    std::string host;
    uint16_t tcpPort = 0;
    uint16_t udpPort = 0;
    std::string gameVersion;
};

BuiltInRendezvousConfig getBuiltInRendezvousConfig();
bool loadBuiltInRendezvousKeys(RendezvousClient::ServerKeys& outKeys, std::string* error = nullptr);

}
