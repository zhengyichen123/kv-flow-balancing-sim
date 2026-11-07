#include "switch-node.h"
#include "ns3/abort.h"
#include "ns3/ipv4-header.h"
#include "ns3/ppp-header.h"
#include "ns3/qbb-channel.h"
#include "ns3/simulator.h"
#include <algorithm>
#include <limits>

namespace ns3 {
namespace {
std::pair<uint64_t, uint32_t> PinKey(const KvBlockHeader& h) {
    return {h.transfer, h.flags & KvBlockHeader::MEGA
        ? std::numeric_limits<uint32_t>::max() : h.block};
}
}

Ptr<Node> SwitchNode::KvNeighbor(uint32_t port) const {
    auto device = m_devices.at(port);
    auto channel = device->GetChannel();
    for (uint32_t n = 0; n < channel->GetNDevices(); ++n) {
        auto peer = channel->GetDevice(n);
        if (peer != device) return peer->GetNode();
    }
    NS_ABORT_MSG("KV routing requires a point-to-point neighbor");
    return nullptr;
}
void SwitchNode::InitializeKvRouting() {
    auto& cfg = KvRoutingConfig::Get();
    m_kvPolicy = KvRoutingPolicy::Create(cfg.mode);
    if (m_kvPolicy->Transport() == KvTransport::LEGACY) return;
    if (m_kvPolicy->UsesLabels()) {
        NS_ABORT_MSG_IF(!m_ecnEnabled, "Author LDLB detector requires EcnEnabled");
        m_ldlbRandom = CreateObject<UniformRandomVariable>();
        m_ldlbRandom->SetStream(static_cast<int64_t>(cfg.seed % 1000000000ULL) + GetId());
    }
    for (uint32_t p = 1; p < m_devices.size(); ++p) {
        auto device = DynamicCast<QbbNetDevice>(m_devices[p]);
        NS_ABORT_MSG_IF(!device || !device->GetDataRate().GetBitRate(),
                        "KV routing requires Qbb links with nonzero bandwidth");
    }
    for (const auto& route : m_rtTable) InitializeKvPropagation(route.first);
    if (m_kvPolicy->UsesFeedback())
        m_kvPeriodic = Simulator::Schedule(NanoSeconds(1), &SwitchNode::EmitKvFeedback, this, true);
    m_kvMaintenance = Simulator::Schedule(NanoSeconds(100000), &SwitchNode::MaintainKvState, this);
}

uint64_t SwitchNode::InitializeKvPropagation(uint32_t destination) {
    const auto route = m_rtTable.find(destination);
    NS_ABORT_MSG_IF(route == m_rtTable.end() || route->second.empty(),
                    "KV propagation requires a reachable destination");
    NS_ABORT_MSG_IF(!m_kvPropagationPending.insert(destination).second,
                    "KV propagation requires an acyclic next-hop routing graph");
    uint64_t minimum = std::numeric_limits<uint64_t>::max();
    for (int port : route->second) {
        const auto key = std::make_pair(destination, port);
        auto cached = m_kvPropagation.find(key);
        if (cached == m_kvPropagation.end()) {
            auto device = DynamicCast<QbbNetDevice>(m_devices.at(port));
            NS_ABORT_MSG_IF(!device || !device->GetDataRate().GetBitRate(),
                            "KV propagation requires Qbb links with nonzero bandwidth");
            auto channel = DynamicCast<QbbChannel>(device->GetChannel());
            NS_ABORT_MSG_IF(!channel || channel->GetDelay().IsStrictlyNegative(),
                            "KV propagation requires a nonnegative Qbb channel delay");
            uint64_t delay = channel->GetDelay().GetNanoSeconds();
            auto neighbor = KvNeighbor(port);
            // 仅初始化时读取静态路由后缀，不读取远端排队或拥塞状态。
            if (neighbor->GetNodeType() != 0) {
                auto next = DynamicCast<SwitchNode>(neighbor);
                NS_ABORT_MSG_IF(!next, "KV next hop is not a supported switch");
                const auto suffix = next->InitializeKvPropagation(destination);
                NS_ABORT_MSG_IF(suffix > std::numeric_limits<uint64_t>::max() - delay,
                                "KV propagation delay overflow");
                delay += suffix;
            }
            cached = m_kvPropagation.emplace(key, delay).first;
        }
        minimum = std::min(minimum, cached->second);
    }
    m_kvPropagationPending.erase(destination);
    return minimum;
}

KvRoutingContext SwitchNode::MakeKvContext(uint32_t destination, const KvBlockHeader* h) {
    KvRoutingContext c;
    c.switchId = GetId(); c.destination = destination;
    c.now = Simulator::Now().GetNanoSeconds();
    const auto& cfg = KvRoutingConfig::Get();
    const uint64_t packets = (cfg.blockBytes + cfg.mtu - 1) / cfg.mtu;
    c.wireBytes = cfg.blockBytes + packets * (36 + KvBlockHeader::SIZE);
    if (h) {
        c.transfer = h->transfer; c.request = h->request;
        c.epoch = h->epoch; c.block = h->block;
        if (h->flags & KvBlockHeader::MEGA) c.wireBytes *= h->parentBlocks;
    }
    for (int port : m_rtTable.at(destination)) {
        KvPortView p;
        p.port = port;
        auto device = DynamicCast<QbbNetDevice>(m_devices[port]);
        p.bandwidth = device->GetDataRate().GetBitRate();
        p.propagation = m_kvPropagation.at({destination, port});
        p.queue = device->GetQueue()->GetNBytesTotal();
        p.up = device->IsLinkUp();
        p.localCongested = m_ldlb.Local(port);
        p.remoteCongested = m_ldlb.Remote(port, destination);
        p.serial = device->GetSerializationBytes();
        p.reserved = m_kvReserved[port];
        p.terminal = KvNeighbor(port)->GetNodeType() == 0;
        if (p.terminal) { p.fresh = true; p.oldest = c.now; }
        else {
            auto found = m_kvCache.find({port, destination});
            if (found != m_kvCache.end()) {
                const auto& cache = found->second;
                p.horizon = cache.record.horizon; p.floor = cache.record.floor;
                p.oldest = cache.record.oldest;
                p.age = c.now - cache.generated;
                p.fresh = (cache.record.flags & KvLoadRecord::FRESH) &&
                          p.age <= cfg.ttlNs && c.now - p.oldest <= cfg.ttlNs;
            }
        }
        c.ports.push_back(p);
    }
    // LDLB 精确平局沿用作者路由表的候选顺序，其他策略保留原有规范化顺序。
    if (!m_kvPolicy || !m_kvPolicy->UsesLabels())
        std::sort(c.ports.begin(), c.ports.end(), [](auto& a, auto& b) { return a.port < b.port; });
    return c;
}

int SwitchNode::SelectKvPort(const CustomHeader& ch) {
    auto& next = m_rtTable.at(ch.dip);
    m_kvLastDecision = false;
    if (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD) {
        auto pin = m_kvPins.find(PinKey(ch.kv));
        const uint64_t target = ch.kv.flags & KvBlockHeader::MEGA
            ? uint64_t(ch.kv.parentBlocks) * KvRoutingConfig::Get().blockBytes
            : KvRoutingConfig::Get().blockBytes;
        if (ch.l3Prot == 0xFC && ch.ack.seq == target && pin != m_kvPins.end()) {
            NS_ABORT_MSG_IF(pin->second.remaining, "Completed KV block has residual reservation");
            pin->second.expiry = Simulator::Now().GetNanoSeconds() +
                                 KvRoutingConfig::Get().tombstoneNs;
        }
        if (next.size() == 1) return next.front();
        for (int p : next)
            if (KvNeighbor(p)->GetId() == ch.kv.cookie) return p;
        NS_ABORT_MSG("KV ACK route cookie has no reachable reverse path");
    }
    auto pin = m_kvPins.find(PinKey(ch.kv));
    if (pin != m_kvPins.end()) return pin->second.port;
    m_kvContext = MakeKvContext(ch.dip, &ch.kv);
    uint32_t words[3] = {ch.sip, ch.dip,
                        uint32_t(ch.udp.sport) | (uint32_t(ch.udp.dport) << 16)};
    m_kvContext.ecmpPort = next[EcmpHash(reinterpret_cast<uint8_t*>(words), 12, m_ecmpSeed) % next.size()];
    if (m_kvPolicy->UsesLabels()) {
        const auto priority = ch.udp.pg;
        NS_ABORT_MSG_IF(priority >= qCnt, "Invalid LDLB data priority");
        for (auto& p : m_kvContext.ports)
            p.queue = std::max<uint64_t>(p.queue, m_mmu->egress_bytes[p.port][priority]);
        // 请求级 LDLB 保留作者的 12 字节地址/端口哈希。
        m_kvContext.ldlbTieHash =
            EcmpHash(reinterpret_cast<uint8_t*>(words), 12, m_ecmpSeed ^ 0x7f4a7c15u);
    }
    m_kvDecision = m_kvPolicy->Select(m_kvContext);
    NS_ABORT_MSG_IF(std::find(next.begin(), next.end(), m_kvDecision.port) == next.end(),
                    "Routing policy returned an unreachable port");
    m_kvLastDecision = true;
    return m_kvDecision.port;
}

void SwitchNode::CommitKvPacket(int port, uint32_t inPort, Ptr<Packet>& packet, CustomHeader& ch) {
    if (ch.l3Prot != 0x11) return;
    const auto& cfg = KvRoutingConfig::Get();
    const auto& h = ch.kv;
    const uint64_t count = (h.parentBytes + cfg.blockBytes - 1) / cfg.blockBytes;
    const uint64_t payload = packet->GetSize() - ch.GetSerializedSize();
    NS_ABORT_MSG_IF(!h.valid || !h.parentBytes || h.parentBlocks != count ||
                    count * cfg.blockBytes > std::numeric_limits<uint32_t>::max() ||
                    h.block >= count || h.offset % cfg.mtu || !payload ||
                    h.offset + payload > cfg.blockBytes,
                    "Invalid KV data descriptor");
    auto [it, inserted] = m_kvPins.try_emplace(PinKey(h));
    auto& pin = it->second;
    const uint64_t perBlock = (cfg.blockBytes + cfg.mtu - 1) / cfg.mtu;
    if (inserted) {
        pin.port = port;
        pin.packetCount = h.flags & KvBlockHeader::MEGA ? perBlock * count : perBlock;
        pin.admitted.resize((pin.packetCount + 63) / 64);
        pin.remaining = (h.flags & KvBlockHeader::MEGA ? count : 1) *
            (cfg.blockBytes + perBlock * (36 + KvBlockHeader::SIZE));
        m_kvReserved[port] += pin.remaining;
        if (m_kvLastDecision && KvRoutingStats::Get().decisions) {
            auto& out = KvRoutingStats::Get().decisions;
            for (const auto& p : m_kvContext.ports)
                out << m_kvContext.now << ',' << GetId() << ',' << h.request << ',' << h.transfer << ','
                    << h.block << ',' << port << ',' << cfg.mode << ',' << m_kvDecision.fallback << ','
                    << m_kvDecision.cost << ',' << p.port << ',' << p.queue << ',' << p.serial << ','
                    << p.reserved << ',' << p.horizon << ',' << p.floor << ',' << p.age << ',' << p.fresh << ','
                    << p.bandwidth << ',' << p.propagation << ','
                    << p.localCongested << ',' << p.remoteCongested << '\n';
        }
    }
    NS_ABORT_MSG_IF(pin.port != port, "KV block changed its pinned path");
    const uint64_t index = (h.flags & KvBlockHeader::MEGA ? perBlock * h.block : 0) +
                           h.offset / cfg.mtu;
    NS_ABORT_MSG_IF(index >= pin.packetCount, "KV packet offset exceeds bitmap");
    const auto bit = uint64_t{1} << (index % 64);
    if (!(pin.admitted[index / 64] & bit)) {
        NS_ABORT_MSG_IF(packet->GetSize() > pin.remaining ||
                        packet->GetSize() > m_kvReserved[port], "KV reservation underflow");
        pin.admitted[index / 64] |= bit;
        pin.remaining -= packet->GetSize();
        m_kvReserved[port] -= packet->GetSize();
    }
    // 用实际序列化头回写 cookie，PacketTag 不代替线上元数据。
    if (KvNeighbor(inPort)->GetNodeType() == 0 && KvNeighbor(port)->GetNodeType() != 0) {
        ch.kv.cookie = KvNeighbor(port)->GetId();
        const auto offset = ch.GetSerializedSize() - KvBlockHeader::SIZE;
        auto prefix = packet->CreateFragment(0, offset);
        packet->RemoveAtStart(offset + KvBlockHeader::SIZE);
        packet->AddHeader(ch.kv);
        prefix->AddAtEnd(packet);
        packet = prefix;
    }
}

void SwitchNode::SampleLdlb(uint32_t port, uint32_t priority) {
    if (!m_kvPolicy || !m_kvPolicy->UsesLabels()) return;
    m_ldlb.Sample(port, priority, m_mmu->egress_bytes[port][priority],
                  m_mmu->kmin[port], m_mmu->kmax[port], m_mmu->pmax[port],
                  m_ldlbRandom->GetValue());
}
void SwitchNode::PollLdlbBroadcast() {
    if (!m_kvPolicy || !m_kvPolicy->UsesLabels()) return;
    const uint64_t now = Simulator::Now().GetNanoSeconds();
    if (!m_ldlb.PollBroadcast(now)) return;
    LdlbLabelHeader header; header.sender = GetId();
    // 任意下一跳被本地标签标记即将该目的地址加入快照（作者代码的并集规则）。
    for (const auto& route : m_rtTable)
        for (int port : route.second)
            if (m_ldlb.Local(port)) { header.destinations.insert(route.first); break; }
    for (uint32_t port = 1; port < m_devices.size(); ++port) {
        if (KvNeighbor(port)->GetNodeType() == 0) continue;
        auto device = DynamicCast<QbbNetDevice>(m_devices[port]);
        if (!device->IsLinkUp()) continue;
        auto packet = Create<Packet>(); packet->AddHeader(header);
        Ipv4Header ip;
        ip.SetProtocol(LdlbLabelHeader::PROTOCOL); ip.SetTtl(1);
        ip.SetDestination(Ipv4Address("255.255.255.255"));
        ip.SetPayloadSize(packet->GetSize()); packet->AddHeader(ip);
        PppHeader ppp; ppp.SetProtocol(0x0021); packet->AddHeader(ppp);
        CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header);
        packet->PeekHeader(ch);
        const auto wire = packet->GetSize();
        device->SwitchSend(0, packet, ch);
        auto& log = KvRoutingStats::Get().feedback;
        if (log) log << now << ',' << GetId() << ',' << port << ",ldlb_send,0,"
                     << wire << ',' << header.destinations.size() << '\n';
    }
}
void SwitchNode::ReceiveLdlbLabel(uint32_t inPort, Ptr<Packet> packet) {
    if (!m_kvPolicy || !m_kvPolicy->UsesLabels()) return;
    const auto wire = packet->GetSize();
    PppHeader ppp; Ipv4Header ip; LdlbLabelHeader header;
    packet->RemoveHeader(ppp); packet->RemoveHeader(ip);
    const auto consumed = packet->RemoveHeader(header);
    const bool accepted = consumed && header.valid && !packet->GetSize() &&
        header.sender == KvNeighbor(inPort)->GetId() && KvNeighbor(inPort)->GetNodeType() != 0;
    if (accepted) m_ldlb.Receive(inPort, header.destinations);
    auto& log = KvRoutingStats::Get().feedback;
    if (log) log << Simulator::Now().GetNanoSeconds() << ',' << GetId() << ',' << inPort
                 << (accepted ? ",ldlb_receive,0," : ",ldlb_reject,0,")
                 << wire << ',' << header.destinations.size() << '\n';
}

void SwitchNode::MarkKvDirty() {
    if (m_kvPolicy && m_kvPolicy->UsesFeedback() && m_kvEvent.IsExpired())
        m_kvEvent = Simulator::Schedule(NanoSeconds(KvRoutingConfig::Get().minGapNs),
                                         &SwitchNode::EmitKvFeedback, this, false);
}
void SwitchNode::EmitKvFeedback(bool periodic) {
    const auto& cfg = KvRoutingConfig::Get();
    const uint64_t now = Simulator::Now().GetNanoSeconds();
    std::map<uint32_t, std::vector<KvLoadRecord>> offered;
    for (const auto& route : m_rtTable) {
        auto record = m_kvPolicy->Advertise(MakeKvContext(route.first));
        for (uint32_t p = 1; p < m_devices.size(); ++p)
            if (KvNeighbor(p)->GetNodeType() != 0 &&
                std::find(route.second.begin(), route.second.end(), p) == route.second.end())
                offered[p].push_back(record);
    }
    for (auto& item : offered) {
        auto& previous = m_kvLastAdvertised[item.first];
        bool changed = periodic || previous.size() != item.second.size();
        for (size_t n = 0; !changed && n < previous.size(); ++n) {
            auto a = previous[n].horizon, b = item.second[n].horizon;
            const auto device = DynamicCast<QbbNetDevice>(m_devices[item.first]);
            const auto threshold = KvRoutingPolicy::ServiceNs(cfg.blockBytes / 8,
                                                               device->GetDataRate().GetBitRate());
            changed = previous[n].flags != item.second[n].flags ||
                      (a > b ? a - b : b - a) >= threshold;
        }
        if (!changed || m_kvPending.count(item.first)) continue;
        m_kvPending[item.first] = {++m_kvSequence, now, item.second, 0};
        previous = item.second;
    }
    DrainKvFeedback();
    if (periodic)
        m_kvPeriodic = Simulator::Schedule(NanoSeconds(cfg.refreshNs),
                                            &SwitchNode::EmitKvFeedback, this, true);
}
void SwitchNode::DrainKvFeedback() {
    const auto& cfg = KvRoutingConfig::Get();
    const uint64_t now = Simulator::Now().GetNanoSeconds();
    constexpr uint32_t perFragment = (4000 - 44 - 22) / 32;
    for (auto it = m_kvPending.begin(); it != m_kvPending.end();) {
        const auto port = it->first;
        auto& pending = it->second;
        auto& bucket = m_kvBuckets[port];
        auto device = DynamicCast<QbbNetDevice>(m_devices[port]);
        auto refill = static_cast<unsigned __int128>(now - bucket.updated) *
                      device->GetDataRate().GetBitRate() * cfg.budgetPpm / 8000000000000000ULL;
        bucket.tokens = std::min<uint64_t>(8000, bucket.tokens + static_cast<uint64_t>(refill));
        bucket.updated = now;
        KvLoadHeader header;
        header.sender = GetId(); header.boot = 1;
        header.sequence = pending.sequence; header.generated = pending.generated;
        header.fragments = (pending.records.size() + perFragment - 1) / perFragment;
        header.fragment = pending.next / perFragment;
        const auto end = std::min<size_t>(pending.records.size(), pending.next + perFragment);
        header.records.assign(pending.records.begin() + pending.next, pending.records.begin() + end);
        const auto wire = header.GetSerializedSize() + 22;
        if (bucket.tokens < wire || (bucket.lastTx && now - bucket.lastTx < cfg.minGapNs)) {
            ++it; continue;
        }
        auto packet = Create<Packet>();
        packet->AddHeader(header);
        Ipv4Header ip;
        ip.SetProtocol(KvLoadHeader::PROTOCOL); ip.SetTtl(1);
        ip.SetPayloadSize(packet->GetSize());
        packet->AddHeader(ip);
        PppHeader ppp; ppp.SetProtocol(0x0021); packet->AddHeader(ppp);
        CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
        packet->PeekHeader(ch);
        bucket.tokens -= wire; bucket.lastTx = now;
        device->SwitchSend(0, packet, ch);
        auto& log = KvRoutingStats::Get().feedback;
        if (log) log << now << ',' << GetId() << ',' << port << ",send,"
                     << header.sequence << ',' << wire << ',' << header.records.size() << '\n';
        pending.next = end;
        if (pending.next == pending.records.size()) it = m_kvPending.erase(it);
        else ++it;
    }
    if (!m_kvPending.empty() && m_kvDrain.IsExpired())
        m_kvDrain = Simulator::Schedule(NanoSeconds(cfg.minGapNs),
                                         &SwitchNode::DrainKvFeedback, this);
}

void SwitchNode::ReceiveKvFeedback(uint32_t inPort, Ptr<Packet> packet) {
    const uint64_t now = Simulator::Now().GetNanoSeconds();
    const auto wire = packet->GetSize();
    PppHeader ppp; Ipv4Header ip; KvLoadHeader header;
    packet->RemoveHeader(ppp); packet->RemoveHeader(ip);
    const bool decoded = packet->RemoveHeader(header) != 0;
    auto& log = KvRoutingStats::Get().feedback;
    const auto report = [&](const char* event) {
        if (log) log << now << ',' << GetId() << ',' << inPort << ',' << event << ','
                     << header.sequence << ',' << wire << ',' << header.records.size() << '\n';
    };
    if (!decoded || !header.valid || header.sender != KvNeighbor(inPort)->GetId() ||
        header.boot != 1 || header.generated > now ||
        header.fragments > (m_rtTable.size() + 121) / 122 ||
        header.sequence <= m_kvReceivedSequence[inPort]) { report("reject"); return; }
    for (const auto& r : header.records) {
        auto route = m_rtTable.find(r.destination);
        if (r.oldest > header.generated || route == m_rtTable.end() ||
            std::find(route->second.begin(), route->second.end(), inPort) == route->second.end()) {
            report("reject"); return;
        }
    }
    auto& assembly = m_kvAssemblies[inPort];
    if (assembly.sequence > header.sequence) { report("old"); return; }
    if (assembly.sequence != header.sequence)
        assembly = {header.sequence, header.generated, header.fragments, {}};
    if (assembly.fragments != header.fragments || assembly.generated != header.generated) {
        report("reject"); return;
    }
    assembly.parts[header.fragment] = header.records;
    if (assembly.parts.size() != assembly.fragments) { report("fragment"); return; }
    std::set<uint32_t> seen;
    for (const auto& part : assembly.parts)
        for (const auto& r : part.second)
            if (!seen.insert(r.destination).second) { report("reject"); return; }
    for (const auto& part : assembly.parts)
        for (const auto& r : part.second) m_kvCache[{inPort, r.destination}] = {r, header.generated};
    m_kvReceivedSequence[inPort] = header.sequence;
    m_kvAssemblies.erase(inPort);
    report("commit");
    MarkKvDirty();
}
void SwitchNode::MaintainKvState() {
    const uint64_t now = Simulator::Now().GetNanoSeconds();
    for (auto it = m_kvPins.begin(); it != m_kvPins.end();) {
        if (it->second.expiry && it->second.expiry <= now) it = m_kvPins.erase(it);
        else ++it;
    }
    auto& out = KvRoutingStats::Get().ports;
    for (uint32_t p = 1; p < m_devices.size(); ++p) {
        auto device = DynamicCast<QbbNetDevice>(m_devices[p]);
        auto q = device->GetQueue()->GetNBytesTotal(), s = device->GetSerializationBytes();
        if (out && (q || s || m_kvReserved[p] || m_txBytes[p] != m_kvSampleBytes[p])) {
            out << now << ',' << GetId() << ',' << p << ',' << m_txBytes[p] << ','
                << q << ',' << s << ',' << m_kvReserved[p] << '\n';
            m_kvSampleBytes[p] = m_txBytes[p];
        }
    }
    m_kvMaintenance = Simulator::Schedule(NanoSeconds(100000), &SwitchNode::MaintainKvState, this);
}
}
