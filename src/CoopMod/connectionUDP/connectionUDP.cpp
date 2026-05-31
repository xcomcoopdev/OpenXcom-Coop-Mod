#include "connectionUDP.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

namespace OpenXcom
{

namespace
{
    static const uint32_t kMagic = 0x4F584355u; // "OXCU"
    static const uint16_t kVersion = 1;
    static const size_t kHeaderSize = 68;
    static const size_t kMaxUdpPacket = 1200;
    static const size_t kTagBytes = crypto_aead_xchacha20poly1305_ietf_ABYTES;
    static const size_t kMaxPlainPerPacket = kMaxUdpPacket - kHeaderSize - kTagBytes;
    static const uint64_t kPunchIntervalMs = 250;
    static const uint64_t kKeepAliveMs = 500;
    static const uint64_t kAckIntervalMs = 15;
    static const uint64_t kStateExpireMs = 30000;

    // Keep the UDP reliable layer from flooding the OS socket buffer during
    // heavy battlescape sync bursts. Flooding caused packet loss, long
    // retransmission backoff chains and visible multi-second stalls even on LAN.
    static const int kMaxBatchNewMessages = 16;
    static const size_t kMaxPendingMessages = 128;
    static const size_t kMaxPendingPackets = 512;
    static const size_t kMaxSendPacketsPerTick = 96;

    // Large fragmented reliable messages are the dangerous case for UDP.
    // A single missing fragment blocks ordered delivery for all following
    // messages. Use fragment-level NACKs to repair only missing fragments and
    // avoid repeatedly flooding the socket with the whole large message.
    static const size_t kLargeFragmentRetransmitThreshold = 32;
    static const size_t kLargeFragmentProbePackets = 8;
    static const uint64_t kFragNackInitialDelayMs = 60;
    static const uint64_t kFragNackIntervalMs = 100;
    static const size_t kMaxFragNackEntries = 256;

    static bool seqLess(uint32_t a, uint32_t b)
    {
        return static_cast<int32_t>(a - b) < 0;
    }

    static size_t retransmitTimeoutMs(int sendCount)
    {
        if (sendCount < 4) return 60;
        if (sendCount < 10) return 120;
        if (sendCount < 25) return 250;
        return 500;
    }
}


connectionUDP::connectionUDP()
{
}

connectionUDP::~connectionUDP()
{
    stop();
}

uint64_t connectionUDP::nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void connectionUDP::writeBE16(unsigned char* p, uint16_t v)
{
    p[0] = static_cast<unsigned char>((v >> 8) & 0xff);
    p[1] = static_cast<unsigned char>(v & 0xff);
}

void connectionUDP::writeBE32(unsigned char* p, uint32_t v)
{
    p[0] = static_cast<unsigned char>((v >> 24) & 0xff);
    p[1] = static_cast<unsigned char>((v >> 16) & 0xff);
    p[2] = static_cast<unsigned char>((v >> 8) & 0xff);
    p[3] = static_cast<unsigned char>(v & 0xff);
}

void connectionUDP::writeBE64(unsigned char* p, uint64_t v)
{
    for (int i = 7; i >= 0; --i)
    {
        p[7 - i] = static_cast<unsigned char>((v >> (i * 8)) & 0xff);
    }
}

uint16_t connectionUDP::readBE16(const unsigned char* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t connectionUDP::readBE32(const unsigned char* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t connectionUDP::readBE64(const unsigned char* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
    {
        v = (v << 8) | p[i];
    }
    return v;
}

bool connectionUDP::sameEndpoint(const IPaddress& a, const IPaddress& b)
{
    return a.host == b.host && a.port == b.port;
}

std::string connectionUDP::endpointToString(const IPaddress& a)
{
    Uint32 host = SDL_SwapBE32(a.host);
    Uint16 port = SDL_SwapBE16(a.port);
    std::ostringstream ss;
    ss << ((host >> 24) & 0xff) << "."
       << ((host >> 16) & 0xff) << "."
       << ((host >> 8) & 0xff) << "."
       << (host & 0xff) << ":" << port;
    return ss.str();
}

void connectionUDP::log(const std::string& s)
{
    if (_cfg.log)
        _cfg.log(s);
}

bool connectionUDP::start(const Config& cfg)
{
    if (_running.load())
        return false;

    if (!cfg.popTx || !cfg.pushRx || cfg.sessionId == 0 || cfg.localPlayerId == 0 || cfg.remotePlayerId == 0)
        return false;

    if (sodium_init() < 0)
        return false;

    _cfg = cfg;
    _stop.store(false);
    _peerReady.store(false);
    _sessionLocked.store(false);
    _rttMs.store(0);
    _hasPeer = false;
    _nextSeq = 1;
    _recvNext = 1;
    _highestCompleted = 0;
    _nextDiagMs = 0;
    _lastDeliveredMs = 0;
    _lastReliableRxMs = 0;
    _statUdpRxPackets = 0;
    _statDecryptFail = 0;
    _statDataPackets = 0;
    _statOldDataPackets = 0;
    _statDuplicateDataPackets = 0;
    _statReassembledMessages = 0;
    _statDeliveredMessages = 0;
    _statPushRxFailed = 0;
    _statTxMessagesQueued = 0;
    _statTxMessagesTooLarge = 0;
    _statTxPacketsSent = 0;
    _statRetransmitPacketsSent = 0;
    _statAckPacketsSent = 0;
    _statExactAckPacketsSent = 0;
    _statAckedMessages = 0;
    _statFragNackSent = 0;
    _statFragNackRx = 0;
    _statFragRepairPkts = 0;

    if (SDLNet_Init() == -1)
    {
        log("SDLNet_Init failed");
        return false;
    }

    _sock = SDLNet_UDP_Open(cfg.localPort);
    if (!_sock)
    {
        log("SDLNet_UDP_Open failed");
        SDLNet_Quit();
        return false;
    }

    _rx = SDLNet_AllocPacket(static_cast<int>(kMaxUdpPacket));
    _tx = SDLNet_AllocPacket(static_cast<int>(kMaxUdpPacket));
    if (!_rx || !_tx)
    {
        log("SDLNet_AllocPacket failed");
        stop();
        return false;
    }

    buildCandidates();

    _running.store(true);
    _thread = std::thread(&connectionUDP::threadMain, this);
    return true;
}

void connectionUDP::stop()
{
    if (!_running.load() && _sock == nullptr)
        return;

    _stop.store(true);

    if (_thread.joinable())
        _thread.join();

    if (_hasPeer && _sock)
    {
        for (int i = 0; i < 3; ++i)
            sendAuthOnly(F_CLOSE, _peer);
    }

    if (_rx)
    {
        SDLNet_FreePacket(_rx);
        _rx = nullptr;
    }
    if (_tx)
    {
        SDLNet_FreePacket(_tx);
        _tx = nullptr;
    }
    if (_sock)
    {
        SDLNet_UDP_Close(_sock);
        _sock = nullptr;
    }

    clearQueues();
    _running.store(false);
    SDLNet_Quit();
}

void connectionUDP::lockSessionToCurrentPeer()
{
    if (_hasPeer)
    {
        _sessionLocked.store(true);
        log(std::string("UDP session locked to ") + endpointToString(_peer));
    }
}

void connectionUDP::clearQueues()
{
    _pending.clear();
    _reassembly.clear();
    _orderedReady.clear();
    _completedSeqs.clear();
    _recvNext = 1;
    _highestCompleted = 0;
}

bool connectionUDP::addCandidate(const std::string& host, uint16_t port)
{
    if (host.empty() || port == 0)
        return false;

    IPaddress ip{};
    if (SDLNet_ResolveHost(&ip, host.c_str(), port) == -1)
        return false;

    for (const auto& c : _candidates)
    {
        if (sameEndpoint(c, ip))
            return true;
    }

    _candidates.push_back(ip);
    return true;
}

void connectionUDP::buildCandidates()
{
    _candidates.clear();
    addCandidate(_cfg.remoteHost, _cfg.remotePort);

    if (!_cfg.enablePortGuessing)
        return;

    const int base = static_cast<int>(_cfg.remotePort);
    const int radius = std::max(0, _cfg.portGuessRadius);
    for (int d = 1; d <= radius; ++d)
    {
        if (base - d > 0)
            addCandidate(_cfg.remoteHost, static_cast<uint16_t>(base - d));
        if (base + d < 65535)
            addCandidate(_cfg.remoteHost, static_cast<uint16_t>(base + d));
    }
}

void connectionUDP::serializeHeader(const WireHeader& h, unsigned char* out) const
{
    size_t o = 0;
    writeBE32(out + o, h.magic); o += 4;
    writeBE16(out + o, h.version); o += 2;
    writeBE16(out + o, h.flags); o += 2;
    writeBE64(out + o, h.sessionId); o += 8;
    writeBE32(out + o, h.senderId); o += 4;
    writeBE32(out + o, h.seq); o += 4;
    writeBE32(out + o, h.ack); o += 4;
    writeBE32(out + o, h.ackBits); o += 4;
    writeBE32(out + o, h.msgId); o += 4;
    writeBE16(out + o, h.fragIndex); o += 2;
    writeBE16(out + o, h.fragCount); o += 2;
    writeBE16(out + o, h.plainLen); o += 2;
    std::memcpy(out + o, h.nonce, sizeof(h.nonce)); o += sizeof(h.nonce);
    out[o++] = 0;
    out[o++] = 0;
}

bool connectionUDP::parseHeader(const unsigned char* data, size_t len, WireHeader& h) const
{
    if (len < kHeaderSize)
        return false;

    size_t o = 0;
    h.magic = readBE32(data + o); o += 4;
    h.version = readBE16(data + o); o += 2;
    h.flags = readBE16(data + o); o += 2;
    h.sessionId = readBE64(data + o); o += 8;
    h.senderId = readBE32(data + o); o += 4;
    h.seq = readBE32(data + o); o += 4;
    h.ack = readBE32(data + o); o += 4;
    h.ackBits = readBE32(data + o); o += 4;
    h.msgId = readBE32(data + o); o += 4;
    h.fragIndex = readBE16(data + o); o += 2;
    h.fragCount = readBE16(data + o); o += 2;
    h.plainLen = readBE16(data + o); o += 2;
    std::memcpy(h.nonce, data + o, sizeof(h.nonce)); o += sizeof(h.nonce);

    if (h.magic != kMagic || h.version != kVersion)
        return false;
    if (h.sessionId != _cfg.sessionId)
        return false;
    if (h.senderId != _cfg.remotePlayerId)
        return false;
    if (h.plainLen > kMaxPlainPerPacket)
        return false;
    return true;
}

std::string connectionUDP::buildPacket(uint16_t flags,
                                          uint32_t seq,
                                          uint32_t msgId,
                                          uint16_t fragIndex,
                                          uint16_t fragCount,
                                          const unsigned char* plain,
                                          size_t plainLen,
                                          uint32_t ackOverride,
                                          uint32_t ackBitsOverride)
{
    if (plainLen > kMaxPlainPerPacket)
        return std::string();

    WireHeader h;
    h.magic = kMagic;
    h.version = kVersion;
    h.flags = static_cast<uint16_t>(flags | F_ACK); // piggyback ACK on every packet
    h.sessionId = _cfg.sessionId;
    h.senderId = _cfg.localPlayerId;
    h.seq = seq;
    if (ackOverride != 0)
    {
        // Explicit ACK is used for duplicate/old reliable packets. This is
        // important because the normal highestCompleted/ackBits window only
        // covers the newest 32 messages. If the sender missed an old ACK,
        // it would otherwise keep retransmitting that old sequence forever
        // and waste the send budget, causing minutes-long stalls.
        h.ack = ackOverride;
        h.ackBits = ackBitsOverride;
    }
    else
    {
        h.ack = _highestCompleted;
        h.ackBits = 0;
        for (int i = 0; i < 32; ++i)
        {
            uint32_t s = _highestCompleted - 1u - static_cast<uint32_t>(i);
            if (hasCompleted(s))
                h.ackBits |= (1u << i);
        }
    }
    h.msgId = msgId;
    h.fragIndex = fragIndex;
    h.fragCount = fragCount;
    h.plainLen = static_cast<uint16_t>(plainLen);
    randombytes_buf(h.nonce, sizeof(h.nonce));

    std::string out;
    out.resize(kHeaderSize + plainLen + kTagBytes);
    serializeHeader(h, reinterpret_cast<unsigned char*>(&out[0]));

    unsigned long long clen = 0;
    int ok = crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char*>(&out[0]) + kHeaderSize,
        &clen,
        plainLen ? plain : nullptr,
        static_cast<unsigned long long>(plainLen),
        reinterpret_cast<const unsigned char*>(&out[0]),
        static_cast<unsigned long long>(kHeaderSize),
        nullptr,
        h.nonce,
        _cfg.sessionKey.data());

    if (ok != 0 || clen != plainLen + kTagBytes)
        return std::string();

    return out;
}

bool connectionUDP::parseAndDecrypt(const UDPpacket* pkt, WireHeader& hdr, std::string& plain)
{
    if (!pkt || pkt->len < static_cast<int>(kHeaderSize + kTagBytes) || pkt->len > static_cast<int>(kMaxUdpPacket))
        return false;

    const unsigned char* data = reinterpret_cast<const unsigned char*>(pkt->data);
    if (!parseHeader(data, static_cast<size_t>(pkt->len), hdr))
        return false;

    const size_t cipherLen = static_cast<size_t>(pkt->len) - kHeaderSize;
    if (cipherLen != static_cast<size_t>(hdr.plainLen) + kTagBytes)
        return false;

    plain.resize(hdr.plainLen);
    unsigned long long mlen = 0;
    int ok = crypto_aead_xchacha20poly1305_ietf_decrypt(
        hdr.plainLen ? reinterpret_cast<unsigned char*>(&plain[0]) : nullptr,
        &mlen,
        nullptr,
        data + kHeaderSize,
        static_cast<unsigned long long>(cipherLen),
        data,
        static_cast<unsigned long long>(kHeaderSize),
        hdr.nonce,
        _cfg.sessionKey.data());

    if (ok != 0 || mlen != hdr.plainLen)
        return false;

    return true;
}

void connectionUDP::sendPacketBytes(const std::string& bytes, const IPaddress& to)
{
    if (!_sock || !_tx || bytes.empty() || bytes.size() > kMaxUdpPacket)
        return;

    _tx->address = to;
    _tx->len = static_cast<int>(bytes.size());
    std::memcpy(_tx->data, bytes.data(), bytes.size());
    SDLNet_UDP_Send(_sock, -1, _tx);
    ++_statTxPacketsSent;
    _lastTxMs = nowMs();
}

void connectionUDP::sendAuthOnly(uint16_t flags, const IPaddress& to)
{
    std::string pkt = buildPacket(flags, 0, 0, 0, 0, nullptr, 0);
    if (flags & F_ACK)
        ++_statAckPacketsSent;
    sendPacketBytes(pkt, to);
}

void connectionUDP::pumpOutgoingJson(uint64_t now)
{
    if (!_hasPeer)
        return;

    // Retransmit first so old reliable data keeps moving before we accept more
    // gameplay JSON from g_txQ.
    sendPendingTimedOut(now);

    size_t pendingPackets = 0;
    for (const auto& kv : _pending)
        pendingPackets += kv.second.packets.size();

    if (_pending.size() >= kMaxPendingMessages || pendingPackets >= kMaxPendingPackets)
        return;

    int count = 0;
    std::string msg;
    while (count < kMaxBatchNewMessages &&
           _pending.size() < kMaxPendingMessages &&
           pendingPackets < kMaxPendingPackets &&
           _cfg.popTx(msg))
    {
        if (msg.empty())
            continue;

        PendingMessage pm;
        pm.seq = _nextSeq++;
        pm.firstSentMs = 0;
        pm.lastSentMs = 0;
        pm.sendCount = 0;
        pm.nextSendIndex = 0;

        const size_t fragCount = (msg.size() + kMaxPlainPerPacket - 1) / kMaxPlainPerPacket;
        if (fragCount == 0 || fragCount > 65535)
        {
            ++_statTxMessagesTooLarge;
            log("UDP reliable message too large");
            continue;
        }

        pm.packets.reserve(fragCount);
        for (size_t i = 0; i < fragCount; ++i)
        {
            const size_t off = i * kMaxPlainPerPacket;
            const size_t n = std::min(kMaxPlainPerPacket, msg.size() - off);
            std::string p = buildPacket(F_DATA,
                                        pm.seq,
                                        pm.seq,
                                        static_cast<uint16_t>(i),
                                        static_cast<uint16_t>(fragCount),
                                        reinterpret_cast<const unsigned char*>(msg.data() + off),
                                        n);
            if (!p.empty())
                pm.packets.push_back(std::move(p));
        }

        if (pm.packets.size() == fragCount)
        {
            pendingPackets += pm.packets.size();
            ++_statTxMessagesQueued;
            _pending[pm.seq] = std::move(pm);
        }

        ++count;
    }

    // Send newly queued messages immediately, but still under the same per-tick
    // packet budget used for retransmits.
    sendPendingTimedOut(now);
}

void connectionUDP::sendPendingTimedOut(uint64_t now)
{
    if (!_hasPeer)
        return;

    size_t budget = kMaxSendPacketsPerTick;

    for (auto& kv : _pending)
    {
        if (budget == 0)
            break;

        PendingMessage& pm = kv.second;
        if (pm.packets.empty())
            continue;

        if (pm.firstSentMs == 0)
        {
            pm.firstSentMs = now;
        }

        const bool largeFragmented = pm.packets.size() > kLargeFragmentRetransmitThreshold;

        // First transmission must send every fragment. If the message is small,
        // retransmit the whole message on timeout. If the message is large, do
        // not keep retransmitting hundreds of fragments as one giant burst;
        // receiver-side FRAG_NACK will request the exact missing fragments.
        if (pm.sendCount == 0 || !largeFragmented)
        {
            const bool sendCycleInProgress = pm.nextSendIndex > 0;
            const bool due = pm.lastSentMs == 0 ||
                             sendCycleInProgress ||
                             now - pm.lastSentMs >= retransmitTimeoutMs(pm.sendCount);

            if (!due)
                continue;

            while (pm.nextSendIndex < pm.packets.size() && budget > 0)
            {
                sendPacketBytes(pm.packets[pm.nextSendIndex], _peer);
                if (pm.sendCount > 0)
                    ++_statRetransmitPacketsSent;
                ++pm.nextSendIndex;
                --budget;
            }

            if (pm.nextSendIndex >= pm.packets.size())
            {
                pm.nextSendIndex = 0;
                pm.lastSentMs = now;
                pm.sendCount++;
            }

            continue;
        }

        // Large fragmented message repair fallback. Send only a small rolling
        // probe on timeout so the peer has traffic to refresh reassembly/NACK
        // state, but avoid flooding the whole message over and over. Missing
        // fragments are repaired by handleFragNack().
        if (now - pm.lastSentMs < retransmitTimeoutMs(pm.sendCount))
            continue;

        const size_t toSend = std::min(kLargeFragmentProbePackets, budget);
        for (size_t i = 0; i < toSend && budget > 0; ++i)
        {
            const size_t idx = pm.nextSendIndex % pm.packets.size();
            sendPacketBytes(pm.packets[idx], _peer);
            ++_statRetransmitPacketsSent;
            ++pm.nextSendIndex;
            --budget;
        }

        pm.lastSentMs = now;
        pm.sendCount++;
    }
}

void connectionUDP::sendAckNow()
{
    if (_hasPeer)
        sendAuthOnly(F_ACK, _peer);
}

void connectionUDP::sendAckFor(uint32_t seq)
{
    if (!_hasPeer || seq == 0)
        return;

    // ACK this exact reliable sequence. The generic ACK packet only reports
    // _highestCompleted plus 32 bits below it, so old retransmitted messages
    // can fall outside that window. Explicit ACK prevents the sender from
    // retransmitting already completed old messages forever.
    std::string pkt = buildPacket(F_ACK, 0, 0, 0, 0, nullptr, 0, seq, 0);
    ++_statAckPacketsSent;
    ++_statExactAckPacketsSent;
    sendPacketBytes(pkt, _peer);
}

void connectionUDP::sendFragNack(uint32_t seq, const std::vector<uint16_t>& missing)
{
    if (!_hasPeer || seq == 0 || missing.empty())
        return;

    // Payload: uint16 count followed by uint16 fragment indexes, big endian.
    // Keep it within one UDP datagram. The receiver will send another NACK if
    // more fragments are still missing after this repair round.
    const size_t count = std::min(missing.size(), kMaxFragNackEntries);
    std::string plain;
    plain.resize(2 + count * 2);
    writeBE16(reinterpret_cast<unsigned char*>(&plain[0]), static_cast<uint16_t>(count));
    for (size_t i = 0; i < count; ++i)
    {
        writeBE16(reinterpret_cast<unsigned char*>(&plain[0]) + 2 + i * 2, missing[i]);
    }

    std::string pkt = buildPacket(F_FRAG_NACK,
                                  seq,
                                  0,
                                  0,
                                  0,
                                  reinterpret_cast<const unsigned char*>(plain.data()),
                                  plain.size());
    if (!pkt.empty())
    {
        ++_statFragNackSent;
        sendPacketBytes(pkt, _peer);
    }
}

void connectionUDP::sendPunches(uint64_t now)
{
    if (_peerReady.load())
        return;
    if (now < _nextPunchMs)
        return;

    _nextPunchMs = now + kPunchIntervalMs;
    for (const auto& c : _candidates)
        sendAuthOnly(F_PUNCH, c);
}

void connectionUDP::sendKeepAlive(uint64_t now)
{
    if (!_hasPeer || !_peerReady.load())
        return;
    if (now < _nextKeepAliveMs)
        return;

    _nextKeepAliveMs = now + kKeepAliveMs;
    sendAuthOnly(F_KEEPALIVE, _peer);
}

void connectionUDP::handleAck(uint32_t ack, uint32_t ackBits)
{
    if (ack != 0)
    {
        auto it = _pending.find(ack);
        if (it != _pending.end())
        {
            const uint64_t now = nowMs();
            if (it->second.firstSentMs != 0)
                _rttMs.store(now - it->second.firstSentMs);
            ++_statAckedMessages;
            _pending.erase(it);
        }
    }

    for (int i = 0; i < 32; ++i)
    {
        if (ackBits & (1u << i))
        {
            uint32_t s = ack - 1u - static_cast<uint32_t>(i);
            auto it = _pending.find(s);
            if (it != _pending.end())
            {
                ++_statAckedMessages;
                _pending.erase(it);
            }
        }
    }
}

bool connectionUDP::hasCompleted(uint32_t seq) const
{
    return _completedSeqs.find(seq) != _completedSeqs.end();
}

void connectionUDP::rememberCompleted(uint32_t seq)
{
    if (seq == 0)
        return;
    _completedSeqs.insert(seq);
    if (seqLess(_highestCompleted, seq))
        _highestCompleted = seq;

    // Keep memory bounded.
    while (_completedSeqs.size() > 512)
        _completedSeqs.erase(_completedSeqs.begin());
}

void connectionUDP::updateAckState(uint32_t completedSeq)
{
    rememberCompleted(completedSeq);
    _nextAckMs = 0; // force quick ACK
}

void connectionUDP::handleFragNack(const WireHeader& hdr, const std::string& plain, uint64_t now)
{
    (void)now;

    if (hdr.seq == 0 || plain.size() < 2)
        return;

    auto it = _pending.find(hdr.seq);
    if (it == _pending.end())
        return;

    PendingMessage& pm = it->second;
    if (pm.packets.empty())
        return;

    const unsigned char* data = reinterpret_cast<const unsigned char*>(plain.data());
    uint16_t count = readBE16(data);
    const size_t available = (plain.size() - 2) / 2;
    count = static_cast<uint16_t>(std::min<size_t>(count, available));

    size_t sent = 0;
    for (uint16_t i = 0; i < count && sent < kMaxSendPacketsPerTick; ++i)
    {
        const uint16_t fragIndex = readBE16(data + 2 + static_cast<size_t>(i) * 2);
        if (fragIndex >= pm.packets.size())
            continue;

        sendPacketBytes(pm.packets[fragIndex], _peer);
        ++_statFragRepairPkts;
        ++_statRetransmitPacketsSent;
        ++sent;
    }

    if (sent != 0)
    {
        ++_statFragNackRx;
        pm.lastSentMs = nowMs();
    }
}

void connectionUDP::handleReliableData(const WireHeader& hdr, const std::string& plain, uint64_t now)
{
    if (hdr.seq == 0 || hdr.msgId == 0 || hdr.fragCount == 0 || hdr.fragIndex >= hdr.fragCount)
        return;

    ++_statDataPackets;
    _lastReliableRxMs = now;

    // If this sequence is older than the next message expected by ordered
    // delivery, it has already been delivered to the game. ACK it explicitly
    // and do not put it back into _orderedReady. Without this guard, old
    // retransmits whose completion record had fallen out of _completedSeqs
    // could poison _orderedReady and keep consuming retransmit/send budget.
    if (seqLess(hdr.seq, _recvNext))
    {
        ++_statOldDataPackets;
        sendAckFor(hdr.seq);
        return;
    }

    if (hasCompleted(hdr.seq) || _orderedReady.find(hdr.seq) != _orderedReady.end())
    {
        ++_statDuplicateDataPackets;
        sendAckFor(hdr.seq);
        return;
    }

    Reassembly& r = _reassembly[hdr.seq];
    if (r.fragCount == 0)
    {
        r.seq = hdr.seq;
        r.fragCount = hdr.fragCount;
        r.frags.resize(hdr.fragCount);
        r.present.resize(hdr.fragCount, false);
        r.firstTouchMs = now;
        r.lastTouchMs = now;
        r.lastNackMs = 0;
        r.nackCount = 0;
    }

    if (r.fragCount != hdr.fragCount)
        return;

    r.lastTouchMs = now;
    if (!r.present[hdr.fragIndex])
    {
        r.frags[hdr.fragIndex] = plain;
        r.present[hdr.fragIndex] = true;
    }

    bool complete = true;
    for (bool b : r.present)
    {
        if (!b)
        {
            complete = false;
            break;
        }
    }

    if (!complete)
    {
        maybeSendFragNack(r, now);
        return;
    }

    std::string full;
    size_t total = 0;
    for (const auto& f : r.frags)
        total += f.size();
    full.reserve(total);
    for (auto& f : r.frags)
        full += f;

    _orderedReady[hdr.seq] = std::move(full);
    ++_statReassembledMessages;
    _reassembly.erase(hdr.seq);
    updateAckState(hdr.seq);

    // Send an immediate exact ACK for this sequence. Do not wait for the
    // periodic ACK packet; if the normal ACK is lost and this sequence later
    // falls outside the 32-bit ACK window, the sender can otherwise keep it
    // pending for a very long time and delay newer gameplay messages.
    sendAckFor(hdr.seq);

    deliverOrdered();
}

void connectionUDP::deliverOrdered()
{
    for (;;)
    {
        auto it = _orderedReady.find(_recvNext);
        if (it == _orderedReady.end())
            break;

        std::string msg = std::move(it->second);
        if (!_cfg.pushRx(std::move(msg)))
        {
            // RX queue full. Put it back and try next loop tick.
            ++_statPushRxFailed;
            _orderedReady[_recvNext] = std::move(msg);
            break;
        }

        ++_statDeliveredMessages;
        _lastDeliveredMs = nowMs();
        rememberCompleted(_recvNext);
        _orderedReady.erase(it);
        ++_recvNext;
        _nextAckMs = 0;
    }
}

void connectionUDP::handleIncoming(const UDPpacket* pkt, uint64_t now)
{
    ++_statUdpRxPackets;

    WireHeader hdr;
    std::string plain;
    if (!parseAndDecrypt(pkt, hdr, plain))
    {
        ++_statDecryptFail;
        return;
    }

    // Endpoint lock: before ready/lock, any valid authenticated packet from the
    // expected player may become the peer endpoint. After lock, only that exact
    // IP:port is accepted.
    if (_hasPeer && _sessionLocked.load() && !sameEndpoint(pkt->address, _peer))
        return;

    if (!_hasPeer || !_peerReady.load())
    {
        _peer = pkt->address;
        _hasPeer = true;
        _peerReady.store(true);
        log(std::string("UDP peer authenticated at ") + endpointToString(_peer));
    }

    _lastRxMs = now;

    if (hdr.flags & F_ACK)
        handleAck(hdr.ack, hdr.ackBits);

    if (hdr.flags & F_CLOSE)
    {
        _stop.store(true);
        return;
    }

    if (hdr.flags & F_PUNCH)
    {
        sendAuthOnly(F_PUNCH | F_ACK, _peer);
        return;
    }

    if (hdr.flags & F_KEEPALIVE)
    {
        if (now >= _nextAckMs)
            sendAckNow();
        return;
    }

    if (hdr.flags & F_FRAG_NACK)
    {
        handleFragNack(hdr, plain, now);
        return;
    }

    if (hdr.flags & F_DATA)
    {
        handleReliableData(hdr, plain, now);
    }
}

void connectionUDP::maybeSendFragNack(Reassembly& r, uint64_t now)
{
    if (!_hasPeer || r.seq == 0 || r.fragCount <= 1 || r.present.empty())
        return;

    if (r.lastNackMs == 0)
    {
        if (now - r.firstTouchMs < kFragNackInitialDelayMs)
            return;
    }
    else if (now - r.lastNackMs < kFragNackIntervalMs)
    {
        return;
    }

    std::vector<uint16_t> missing;
    missing.reserve(std::min<size_t>(r.present.size(), kMaxFragNackEntries));
    for (size_t i = 0; i < r.present.size() && missing.size() < kMaxFragNackEntries; ++i)
    {
        if (!r.present[i])
            missing.push_back(static_cast<uint16_t>(i));
    }

    if (missing.empty())
        return;

    sendFragNack(r.seq, missing);
    r.lastNackMs = now;
    ++r.nackCount;
}

void connectionUDP::checkReassemblyNacks(uint64_t now)
{
    for (auto& kv : _reassembly)
    {
        maybeSendFragNack(kv.second, now);
    }
}

void connectionUDP::expireOldState(uint64_t now)
{
    for (auto it = _reassembly.begin(); it != _reassembly.end(); )
    {
        if (now - it->second.lastTouchMs > kStateExpireMs)
            it = _reassembly.erase(it);
        else
            ++it;
    }

    if (_hasPeer && _lastRxMs != 0 && now - _lastRxMs > 15000)
    {
        // The peer was previously authenticated, but then no authenticated UDP
        // packet was received for too long. This usually means the remote
        // player force-closed the game or lost network connectivity and cannot
        // send the normal F_CLOSE packet.
        //
        // Stop the worker thread so connection_udp_glue.cpp can detect
        // isRunning() == false and call handleUdpRemotePeerLost(). That path
        // sets onConnect = -2 for a remote disconnect and lets the host relist
        // the room instead of waiting forever.
        log("UDP peer timed out; treating as remote disconnect");
        _peerReady.store(false);
        _sessionLocked.store(false);
        _stop.store(true);
        return;
    }
}


size_t connectionUDP::pendingPacketCount() const
{
    size_t count = 0;
    for (const auto& kv : _pending)
        count += kv.second.packets.size();
    return count;
}

void connectionUDP::logDiagnostics(uint64_t now)
{
    if (!_cfg.log)
        return;

    if (_nextDiagMs != 0 && now < _nextDiagMs)
        return;

    _nextDiagMs = now + 5000;

    const bool hasBacklog = !_pending.empty() || !_orderedReady.empty() || !_reassembly.empty();
    const bool rxQueueBlocked = _statPushRxFailed != 0;
    const bool noDeliveryWhileReceiving = _lastReliableRxMs != 0 &&
        (_lastDeliveredMs == 0 || now - _lastDeliveredMs > 5000) &&
        now - _lastReliableRxMs < 5000;

    // Keep normal gameplay quiet. Only print when there is visible backlog,
    // RX queue pressure, or data is being received but not delivered to the game.
    if (!hasBacklog && !rxQueueBlocked && !noDeliveryWhileReceiving)
        return;

    uint32_t oldestPendingSeq = 0;
    uint64_t oldestPendingAgeMs = 0;
    int oldestSendCount = 0;
    if (!_pending.empty())
    {
        const auto& pm = _pending.begin()->second;
        oldestPendingSeq = pm.seq;
        oldestSendCount = pm.sendCount;
        if (pm.firstSentMs != 0 && now >= pm.firstSentMs)
            oldestPendingAgeMs = now - pm.firstSentMs;
    }

    uint32_t firstOrderedReadySeq = 0;
    if (!_orderedReady.empty())
        firstOrderedReadySeq = _orderedReady.begin()->first;

    uint32_t firstReassemblySeq = 0;
    uint16_t firstReassemblyHave = 0;
    uint16_t firstReassemblyNeed = 0;
    uint64_t firstReassemblyAgeMs = 0;
    uint32_t firstReassemblyNackCount = 0;
    if (!_reassembly.empty())
    {
        const auto& r = _reassembly.begin()->second;
        firstReassemblySeq = r.seq;
        firstReassemblyNeed = r.fragCount;
        firstReassemblyHave = static_cast<uint16_t>(std::count(r.present.begin(), r.present.end(), true));
        firstReassemblyNackCount = r.nackCount;
        if (now >= r.lastTouchMs)
            firstReassemblyAgeMs = now - r.lastTouchMs;
    }

    const uint64_t deliveredIdleMs = _lastDeliveredMs == 0 ? 0 : now - _lastDeliveredMs;
    const uint64_t rxIdleMs = _lastReliableRxMs == 0 ? 0 : now - _lastReliableRxMs;

    std::ostringstream ss;
    ss << "UDP DIAG peer=" << (_peerReady.load() ? 1 : 0)
       << " pendingMsgs=" << _pending.size()
       << " pendingPkts=" << pendingPacketCount()
       << " orderedReady=" << _orderedReady.size()
       << " reassembly=" << _reassembly.size()
       << " recvNext=" << _recvNext
       << " highestCompleted=" << _highestCompleted
       << " nextSeq=" << _nextSeq
       << " oldPendingSeq=" << oldestPendingSeq
       << " oldPendingAgeMs=" << oldestPendingAgeMs
       << " oldPendingSendCount=" << oldestSendCount
       << " firstOrderedSeq=" << firstOrderedReadySeq
       << " firstReassemblySeq=" << firstReassemblySeq
       << " reassemblyHave=" << firstReassemblyHave
       << " reassemblyNeed=" << firstReassemblyNeed
       << " reassemblyIdleMs=" << firstReassemblyAgeMs
       << " reassemblyNacks=" << firstReassemblyNackCount
       << " deliveredIdleMs=" << deliveredIdleMs
       << " reliableRxIdleMs=" << rxIdleMs
       << " udpRx=" << _statUdpRxPackets
       << " decryptFail=" << _statDecryptFail
       << " dataPkts=" << _statDataPackets
       << " oldData=" << _statOldDataPackets
       << " dupData=" << _statDuplicateDataPackets
       << " reassembled=" << _statReassembledMessages
       << " delivered=" << _statDeliveredMessages
       << " pushRxFail=" << _statPushRxFailed
       << " txQueued=" << _statTxMessagesQueued
       << " txTooLarge=" << _statTxMessagesTooLarge
       << " txPkts=" << _statTxPacketsSent
       << " retransPkts=" << _statRetransmitPacketsSent
       << " ackPkts=" << _statAckPacketsSent
       << " exactAckPkts=" << _statExactAckPacketsSent
       << " ackedMsgs=" << _statAckedMessages
       << " fragNackSent=" << _statFragNackSent
       << " fragNackRx=" << _statFragNackRx
       << " fragRepairPkts=" << _statFragRepairPkts;

    log(ss.str());
}

void connectionUDP::threadMain()
{
    _nextPunchMs = 0;
    _nextKeepAliveMs = 0;
    _nextAckMs = 0;
    _lastRxMs = nowMs();

    while (!_stop.load())
    {
        const uint64_t now = nowMs();

        while (SDLNet_UDP_Recv(_sock, _rx) > 0)
            handleIncoming(_rx, now);

        sendPunches(now);
        sendKeepAlive(now);
        pumpOutgoingJson(now);
        deliverOrdered();
        checkReassemblyNacks(now);

        if (_hasPeer && now >= _nextAckMs)
        {
            _nextAckMs = now + kAckIntervalMs;
            sendAckNow();
        }

        expireOldState(now);
        logDiagnostics(now);
        SDL_Delay(1);
    }

    _running.store(false);
}

} 
