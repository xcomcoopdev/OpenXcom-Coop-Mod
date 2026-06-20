#include "rendezvous_config.h"

#include <sodium.h>

#include <cstring>

namespace OpenXcom
{

const char* kRendezvousHost = "";
const uint16_t kRendezvousTcpPort = 0;
const uint16_t kRendezvousUdpPort = 0;
const char* kRendezvousGameVersion = "1.8.3 [v2026-06-20]";

static const char* kServerBoxPublicKeyBase64 =
	""; // PASTE_SERVER_BOX_PUBLIC_KEY_BASE64_HERE

static const char* kServerSignPublicKeyBase64 =
	""; // PASTE_SERVER_SIGN_PUBLIC_KEY_BASE64_HERE

BuiltInRendezvousConfig getBuiltInRendezvousConfig()
{
    BuiltInRendezvousConfig cfg;
    cfg.host = kRendezvousHost;
    cfg.tcpPort = kRendezvousTcpPort;
    cfg.udpPort = kRendezvousUdpPort;
    cfg.gameVersion = kRendezvousGameVersion;
    return cfg;
}

static bool isPlaceholder(const char* s)
{
    return !s || std::strstr(s, "PASTE_") != nullptr;
}

static bool decodeBase64Key(const char* fieldName,
                            const char* base64,
                            unsigned char* out,
                            size_t outSize,
                            std::string* error)
{
    if (isPlaceholder(base64))
    {
        if (error)
            *error = std::string("Built-in rendezvous key not configured: ") + fieldName;
        return false;
    }

    size_t decodedLen = 0;
    if (sodium_base642bin(out,
                          outSize,
                          base64,
                          std::strlen(base64),
                          nullptr,
                          &decodedLen,
                          nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0 ||
        decodedLen != outSize)
    {
        if (error)
            *error = std::string("Invalid built-in rendezvous key: ") + fieldName;
        return false;
    }

    return true;
}

bool loadBuiltInRendezvousKeys(RendezvousClient::ServerKeys& outKeys, std::string* error)
{
    if (sodium_init() < 0)
    {
        if (error)
            *error = "libsodium init failed";
        return false;
    }

    if (!decodeBase64Key("server_box_public_key",
                         kServerBoxPublicKeyBase64,
                         outKeys.serverBoxPublicKey.data(),
                         outKeys.serverBoxPublicKey.size(),
                         error))
    {
        return false;
    }

    if (!decodeBase64Key("server_sign_public_key",
                         kServerSignPublicKeyBase64,
                         outKeys.serverSignPublicKey.data(),
                         outKeys.serverSignPublicKey.size(),
                         error))
    {
        return false;
    }

    return true;
}

} 
