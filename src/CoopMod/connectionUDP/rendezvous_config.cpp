#include "rendezvous_config.h"

#include "../../Engine/CrossPlatform.h"
#include "../../Engine/Options.h"

#include <sodium.h>
#include <json/json.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>

namespace OpenXcom
{

namespace
{
    struct RendezvousServerEntry
    {
        std::string name;
        std::string host;
        uint16_t tcpPort = 0;
        uint16_t udpPort = 0;
        std::string gameVersion;
        std::string serverBoxPublicKeyB64;
        std::string serverSignPublicKeyB64;
    };

    std::once_flag g_loadOnce;
    std::vector<RendezvousServerEntry> g_servers;
    std::string g_loadedPath;
    std::atomic<size_t> g_activeServer(0);

    std::string joinPath(const std::string& folder, const std::string& file)
    {
        if (folder.empty())
            return file;
        const char last = folder.back();
        if (last == '/' || last == '\\')
            return folder + file;
        return folder + "/" + file;
    }

    // Returns the ordered list of candidate config paths.
    std::vector<std::string> candidatePaths()
    {
        std::vector<std::string> paths;

        if (const char* env = std::getenv("OXC_RENDEZVOUS_CONFIG"))
        {
            if (env[0] != '\0')
                paths.push_back(env);
        }

        // Portable: next to the executable. Easiest for testing/deployment.
        paths.push_back(joinPath(CrossPlatform::getExeFolder(), "rendezvous.json"));

        // Alongside the game's other settings.
        paths.push_back(joinPath(Options::getConfigFolder(), "rendezvous.json"));

        return paths;
    }

    void parseInto(std::istream& in, std::vector<RendezvousServerEntry>& out)
    {
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errs;
        if (!Json::parseFromStream(builder, in, &root, &errs))
            return; // malformed file -> no servers (callers report "not configured")

        const Json::Value& servers = root["servers"];
        if (!servers.isArray())
            return;

        for (Json::ArrayIndex i = 0; i < servers.size(); ++i)
        {
            const Json::Value& s = servers[i];

            RendezvousServerEntry e;
            e.name = s.get("name", "").asString();
            e.host = s.get("host", "").asString();
            e.tcpPort = static_cast<uint16_t>(s.get("tcpPort", 0).asUInt());
            e.udpPort = static_cast<uint16_t>(s.get("udpPort", 0).asUInt());
            e.gameVersion = s.get("gameVersion", "").asString();
            e.serverBoxPublicKeyB64 = s.get("serverBoxPublicKey", "").asString();
            e.serverSignPublicKeyB64 = s.get("serverSignPublicKey", "").asString();

            if (e.name.empty())
                e.name = "Server " + std::to_string(i + 1);

            out.push_back(std::move(e));
        }
    }

    void loadImpl()
    {
        const std::vector<std::string> paths = candidatePaths();
        if (!paths.empty())
            g_loadedPath = paths.front();

        for (const std::string& path : paths)
        {
            std::ifstream f(path.c_str(), std::ios::binary);
            if (!f)
                continue;

            parseInto(f, g_servers);
            g_loadedPath = path;
            return;
        }
        // No file found: g_servers stays empty and callers report "not configured".
    }

    const std::vector<RendezvousServerEntry>& servers()
    {
        std::call_once(g_loadOnce, loadImpl);
        return g_servers;
    }

    bool decodeBase64Key(const char* fieldName,
                         const std::string& base64,
                         unsigned char* out,
                         size_t outSize,
                         std::string* error)
    {
        if (base64.empty())
        {
            if (error)
                *error = std::string("Rendezvous key not configured: ") + fieldName;
            return false;
        }

        size_t decodedLen = 0;
        if (sodium_base642bin(out,
                              outSize,
                              base64.c_str(),
                              base64.size(),
                              nullptr,
                              &decodedLen,
                              nullptr,
                              sodium_base64_VARIANT_ORIGINAL) != 0 ||
            decodedLen != outSize)
        {
            if (error)
                *error = std::string("Invalid rendezvous key: ") + fieldName;
            return false;
        }

        return true;
    }

    bool decodeKeysFor(const RendezvousServerEntry& e,
                       RendezvousClient::ServerKeys& outKeys,
                       std::string* error)
    {
        if (sodium_init() < 0)
        {
            if (error)
                *error = "libsodium init failed";
            return false;
        }

        if (!decodeBase64Key("server_box_public_key",
                             e.serverBoxPublicKeyB64,
                             outKeys.serverBoxPublicKey.data(),
                             outKeys.serverBoxPublicKey.size(),
                             error))
        {
            return false;
        }

        if (!decodeBase64Key("server_sign_public_key",
                             e.serverSignPublicKeyB64,
                             outKeys.serverSignPublicKey.data(),
                             outKeys.serverSignPublicKey.size(),
                             error))
        {
            return false;
        }

        return true;
    }

    // Active index clamped to a valid range for the current server list.
    size_t activeIndex()
    {
        const size_t count = servers().size();
        if (count == 0)
            return 0;
        size_t idx = g_activeServer.load();
        if (idx >= count)
            idx = 0;
        return idx;
    }
}

std::vector<std::string> getRendezvousServerNames()
{
    std::vector<std::string> names;
    for (const auto& e : servers())
        names.push_back(e.name);
    return names;
}

size_t getRendezvousServerCount()
{
    return servers().size();
}

size_t getActiveRendezvousServer()
{
    return activeIndex();
}

std::string getActiveRendezvousServerName()
{
    const auto& list = servers();
    if (list.empty())
        return std::string();
    return list[activeIndex()].name;
}

void setActiveRendezvousServer(size_t index)
{
    const size_t count = servers().size();
    if (count == 0)
        return;
    if (index >= count)
        index = 0;
    g_activeServer.store(index);
}

void setActiveRendezvousServerByName(const std::string& name)
{
    const auto& list = servers();
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (list[i].name == name)
        {
            g_activeServer.store(i);
            return;
        }
    }
    // Unknown/empty name: fall back to the first configured server.
    g_activeServer.store(0);
}

bool getRendezvousServerConfig(size_t index,
                               BuiltInRendezvousConfig& outCfg,
                               RendezvousClient::ServerKeys& outKeys,
                               std::string* error)
{
    const auto& list = servers();
    if (index >= list.size())
    {
        if (error)
            *error = "Rendezvous server index out of range";
        return false;
    }

    const RendezvousServerEntry& e = list[index];
    outCfg.host = e.host;
    outCfg.tcpPort = e.tcpPort;
    outCfg.udpPort = e.udpPort;
    outCfg.gameVersion = e.gameVersion;

    return decodeKeysFor(e, outKeys, error);
}

BuiltInRendezvousConfig getBuiltInRendezvousConfig()
{
    BuiltInRendezvousConfig cfg;
    const auto& list = servers();
    if (list.empty())
        return cfg;

    const RendezvousServerEntry& e = list[activeIndex()];
    cfg.host = e.host;
    cfg.tcpPort = e.tcpPort;
    cfg.udpPort = e.udpPort;
    cfg.gameVersion = e.gameVersion;
    return cfg;
}

bool loadBuiltInRendezvousKeys(RendezvousClient::ServerKeys& outKeys, std::string* error)
{
    const auto& list = servers();
    if (list.empty())
    {
        if (error)
            *error = "No rendezvous servers configured";
        return false;
    }

    return decodeKeysFor(list[activeIndex()], outKeys, error);
}

std::string getRendezvousConfigPath()
{
    servers();
    return g_loadedPath;
}

}
