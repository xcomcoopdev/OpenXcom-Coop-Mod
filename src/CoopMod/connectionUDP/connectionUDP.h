#pragma once

/*
 * UDP transport for SDL_net 1.2
 *
 * Features:
 *  - UDP hole punching candidate probing
 *  - reliable ordered delivery for gameplay JSON messages
 *  - ACK + retransmit until acknowledged while session is alive
 *  - message fragmentation/reassembly for UDP-safe packets
 *  - authenticated encryption with libsodium XChaCha20-Poly1305
 *
 * This class intentionally knows nothing about OpenXcom game states.
 * Feed it outgoing JSON strings with enqueueReliable(), and pull ordered
 * incoming JSON through the rx callback.
 */

#include <SDL_net.h>
#include <sodium.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace OpenXcom
{

class connectionUDP
{
public:
    static const size_t kSessionKeyBytes = crypto_aead_xchacha20poly1305_ietf_KEYBYTES;

    struct Config
    {
        // Local UDP port. Use 0 to let OS select the port.
        uint16_t localPort = 0;

        // Remote public endpoint returned by rendezvous server.
        // For symmetric NAT guessing, remotePort is the observed port and
        // portGuessRadius controls +/- probing around it.
        std::string remoteHost;
        uint16_t remotePort = 0;
        int portGuessRadius = 32;
        bool enablePortGuessing = true;

        // Session values must come from authenticated rendezvous.
        uint64_t sessionId = 0;
        uint32_t localPlayerId = 0;
        uint32_t remotePlayerId = 0;
        std::array<unsigned char, kSessionKeyBytes> sessionKey{};

        // Called by the transport thread to fetch outgoing JSON messages.
        // Return true when a message was written into out.
        std::function<bool(std::string& out)> popTx;

        // Called by the transport thread when a complete, ordered JSON message arrives.
        // Return false if the game RX queue is full; transport will retry later.
        std::function<bool(std::string&& msg)> pushRx;

        // Optional logging hook.
        std::function<void(const std::string&)> log;
    };

    connectionUDP();
    ~connectionUDP();

    connectionUDP(const connectionUDP&) = delete;
    connectionUDP& operator=(const connectionUDP&) = delete;

    bool start(const Config& cfg);
    void stop();

    // After both players are ready, call this. From then on only the current
    // authenticated peer endpoint is accepted; all other IP:port pairs are dropped.
    void lockSessionToCurrentPeer();

    // Drop pending packets and reassembly state. Does not close the socket.
    void clearQueues();

    bool isRunning() const { return _running.load(); }
    bool isPeerReady() const { return _peerReady.load(); }
    uint64_t currentRttMs() const { return _rttMs.load(); }

private:
    enum PacketFlags : uint16_t
    {
        F_ACK       = 1 << 0,
        F_DATA      = 1 << 1,
        F_PUNCH     = 1 << 2,
        F_KEEPALIVE = 1 << 3,
        F_CLOSE     = 1 << 4,
        F_FRAG_NACK = 1 << 5
    };

    struct WireHeader
    {
        uint32_t magic = 0;
        uint16_t version = 0;
        uint16_t flags = 0;
        uint64_t sessionId = 0;
        uint32_t senderId = 0;
        uint32_t seq = 0;      // reliable message order id, 0 for non-data
        uint32_t ack = 0;      // highest completed incoming message seen by sender
        uint32_t ackBits = 0;  // selective ACK bits below ack
        uint32_t msgId = 0;    // same as seq for fragmented reliable messages
        uint16_t fragIndex = 0;
        uint16_t fragCount = 0;
        uint16_t plainLen = 0;
        unsigned char nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES]{};
    };

    struct PendingMessage
    {
        uint32_t seq = 0;
        std::vector<std::string> packets; // encrypted UDP packets, all fragments
        uint64_t firstSentMs = 0;
        uint64_t lastSentMs = 0;
        int sendCount = 0;
        size_t nextSendIndex = 0;
    };

    struct Reassembly
    {
        uint32_t seq = 0;
        uint16_t fragCount = 0;
        std::vector<std::string> frags;
        std::vector<bool> present;
        uint64_t firstTouchMs = 0;
        uint64_t lastTouchMs = 0;
        uint64_t lastNackMs = 0;
        uint32_t nackCount = 0;
    };

    static uint64_t nowMs();
    static void writeBE16(unsigned char* p, uint16_t v);
    static void writeBE32(unsigned char* p, uint32_t v);
    static void writeBE64(unsigned char* p, uint64_t v);
    static uint16_t readBE16(const unsigned char* p);
    static uint32_t readBE32(const unsigned char* p);
    static uint64_t readBE64(const unsigned char* p);

    static bool sameEndpoint(const IPaddress& a, const IPaddress& b);
    static std::string endpointToString(const IPaddress& a);

    void threadMain();
    void log(const std::string& s);

    bool addCandidate(const std::string& host, uint16_t port);
    void buildCandidates();

    std::string buildPacket(uint16_t flags,
                            uint32_t seq,
                            uint32_t msgId,
                            uint16_t fragIndex,
                            uint16_t fragCount,
                            const unsigned char* plain,
                            size_t plainLen,
                            uint32_t ackOverride = 0,
                            uint32_t ackBitsOverride = 0);

    bool parseAndDecrypt(const UDPpacket* pkt, WireHeader& hdr, std::string& plain);
    void serializeHeader(const WireHeader& h, unsigned char* out) const;
    bool parseHeader(const unsigned char* data, size_t len, WireHeader& h) const;

    void pumpOutgoingJson(uint64_t now);
    void sendPendingTimedOut(uint64_t now);
    void sendPacketBytes(const std::string& bytes, const IPaddress& to);
    void sendAuthOnly(uint16_t flags, const IPaddress& to);
    void sendAckNow();
    void sendAckFor(uint32_t seq);
    void sendFragNack(uint32_t seq, const std::vector<uint16_t>& missing);
    void sendPunches(uint64_t now);
    void sendKeepAlive(uint64_t now);

    void handleIncoming(const UDPpacket* pkt, uint64_t now);
    void handleAck(uint32_t ack, uint32_t ackBits);
    void handleFragNack(const WireHeader& hdr, const std::string& plain, uint64_t now);
    void handleReliableData(const WireHeader& hdr, const std::string& plain, uint64_t now);
    void deliverOrdered();
    void rememberCompleted(uint32_t seq);
    bool hasCompleted(uint32_t seq) const;
    void updateAckState(uint32_t completedSeq);
    void expireOldState(uint64_t now);
    void checkReassemblyNacks(uint64_t now);
    void maybeSendFragNack(Reassembly& r, uint64_t now);
    size_t pendingPacketCount() const;
    void logDiagnostics(uint64_t now);

private:
    Config _cfg;

    UDPsocket _sock = nullptr;
    UDPpacket* _rx = nullptr;
    UDPpacket* _tx = nullptr;

    std::thread _thread;
    std::atomic<bool> _running{false};
    std::atomic<bool> _stop{false};
    std::atomic<bool> _peerReady{false};
    std::atomic<bool> _sessionLocked{false};
    std::atomic<uint64_t> _rttMs{0};

    std::vector<IPaddress> _candidates;
    IPaddress _peer{};
    bool _hasPeer = false;

    uint32_t _nextSeq = 1;
    uint32_t _recvNext = 1;
    uint32_t _highestCompleted = 0;
    std::set<uint32_t> _completedSeqs;

    std::map<uint32_t, PendingMessage> _pending;
    std::map<uint32_t, Reassembly> _reassembly;
    std::map<uint32_t, std::string> _orderedReady;

    uint64_t _nextPunchMs = 0;
    uint64_t _nextKeepAliveMs = 0;
    uint64_t _nextAckMs = 0;
    uint64_t _lastRxMs = 0;
    uint64_t _lastTxMs = 0;

    // Low-frequency diagnostics for UDP stalls. These are updated only from
    // the UDP worker thread and logged at most once every few seconds.
    uint64_t _nextDiagMs = 0;
    uint64_t _lastDeliveredMs = 0;
    uint64_t _lastReliableRxMs = 0;

    uint64_t _statUdpRxPackets = 0;
    uint64_t _statDecryptFail = 0;
    uint64_t _statDataPackets = 0;
    uint64_t _statOldDataPackets = 0;
    uint64_t _statDuplicateDataPackets = 0;
    uint64_t _statReassembledMessages = 0;
    uint64_t _statDeliveredMessages = 0;
    uint64_t _statPushRxFailed = 0;
    uint64_t _statTxMessagesQueued = 0;
    uint64_t _statTxMessagesTooLarge = 0;
    uint64_t _statTxPacketsSent = 0;
    uint64_t _statRetransmitPacketsSent = 0;
    uint64_t _statAckPacketsSent = 0;
    uint64_t _statExactAckPacketsSent = 0;
    uint64_t _statAckedMessages = 0;
    uint64_t _statFragNackSent = 0;
    uint64_t _statFragNackRx = 0;
    uint64_t _statFragRepairPkts = 0;
};

} 
