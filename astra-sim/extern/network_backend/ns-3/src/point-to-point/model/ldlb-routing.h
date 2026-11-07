#ifndef LDLB_ROUTING_H
#define LDLB_ROUTING_H

#include "kv-routing-policy.h"
#include "ns3/header.h"
#include <set>

namespace ns3 {

// 作者公开代码的 /24 连续后缀压缩格式；独立协议号避免与 CNP 冲突。
class LdlbLabelHeader : public Header {
public:
    static constexpr uint8_t PROTOCOL = 0xf9;
    uint32_t sender = 0;
    std::set<uint32_t> destinations;
    bool valid = false;
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator) const override;
    uint32_t Deserialize(Buffer::Iterator) override;
    void Print(std::ostream&) const override;
};

// 标签按接口共享，EWMA/连续采样计数按接口和优先级保存，与作者 MMU 一致。
class LdlbState {
public:
    void Sample(uint32_t port, uint32_t priority, uint64_t bytes,
                uint64_t kmin, uint64_t kmax, double pmax, double random);
    bool Local(uint32_t port) const { return m_local.count(port); }
    bool Remote(uint32_t port, uint32_t dip) const;
    void Receive(uint32_t port, const std::set<uint32_t>& destinations);
    bool PollBroadcast(uint64_t now);
    const std::set<uint32_t>& LocalPorts() const { return m_local; }
private:
    struct Detector { double ewma = 0; uint64_t enter = 0, exit = 0; };
    std::map<std::pair<uint32_t, uint32_t>, Detector> m_samples;
    std::set<uint32_t> m_local, m_pending, m_advertised;
    std::map<uint32_t, std::set<uint32_t>> m_remote;
    uint64_t m_rounds = 0, m_lastBroadcast = 0;
};

class LdlbPolicy final : public KvRoutingPolicy {
public:
    KvTransport Transport() const override { return KvTransport::REQUEST; }
    bool UsesLabels() const override { return true; }
    KvDecision Select(const KvRoutingContext&) const override;
};
}
#endif
