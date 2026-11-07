#include "ldlb-routing.h"
// LDLB 逻辑适配自 xuruiiiii/LDLB-NS3，提交 39bcd806a476f37db2667ac894fd084a96c69ed6。
// 上游许可证 GPL-2.0；来源函数与适配边界详见 BLOCK_ROUTING_V1.md 第 24 节。
#include <algorithm>
#include <limits>

namespace ns3 {
namespace {
using Ranges = std::map<uint32_t, std::vector<std::pair<uint8_t, uint8_t>>>;
Ranges Compress(const std::set<uint32_t>& destinations) {
    Ranges result;
    for (auto dip : destinations) {
        auto& ranges = result[dip & 0xffffff00u];
        const auto suffix = static_cast<uint8_t>(dip);
        if (!ranges.empty() && unsigned(ranges.back().second) + 1 == suffix)
            ranges.back().second = suffix;
        else ranges.emplace_back(suffix, suffix);
    }
    return result;
}
}
NS_OBJECT_ENSURE_REGISTERED(LdlbLabelHeader);
TypeId LdlbLabelHeader::GetTypeId() {
    static TypeId t = TypeId("ns3::LdlbLabelHeader").SetParent<Header>()
        .AddConstructor<LdlbLabelHeader>();
    return t;
}
TypeId LdlbLabelHeader::GetInstanceTypeId() const { return GetTypeId(); }
uint32_t LdlbLabelHeader::GetSerializedSize() const {
    uint32_t size = 8;
    for (const auto& item : Compress(destinations)) size += 8 + 2 * item.second.size();
    return size;
}
void LdlbLabelHeader::Serialize(Buffer::Iterator i) const {
    const auto prefixes = Compress(destinations);
    i.WriteHtonU32(sender); i.WriteHtonU32(prefixes.size());
    for (const auto& item : prefixes) {
        i.WriteHtonU32(item.first); i.WriteHtonU32(item.second.size());
        for (const auto& range : item.second) {
            i.WriteU8(range.first); i.WriteU8(range.second);
        }
    }
}
uint32_t LdlbLabelHeader::Deserialize(Buffer::Iterator i) {
    valid = false; destinations.clear();
    const auto begin = i;
    if (i.GetRemainingSize() < 8) return 0;
    sender = i.ReadNtohU32();
    const auto count = i.ReadNtohU32();
    if (count > i.GetRemainingSize() / 10) return 0;
    std::set<uint32_t> seen;
    std::set<uint32_t> decoded;
    for (uint32_t n = 0; n < count; ++n) {
        if (i.GetRemainingSize() < 8) return 0;
        const auto prefix = i.ReadNtohU32(), ranges = i.ReadNtohU32();
        if ((prefix & 255) || !seen.insert(prefix).second || !ranges ||
            ranges > 128 || ranges > i.GetRemainingSize() / 2) return 0;
        int previous = -2;
        for (uint32_t r = 0; r < ranges; ++r) {
            const auto low = i.ReadU8(), high = i.ReadU8();
            if (low > high || low <= previous + 1) return 0;
            for (uint32_t suffix = low; suffix <= high; ++suffix)
                decoded.insert(prefix | suffix);
            previous = high;
        }
    }
    destinations = std::move(decoded); valid = true;
    return i.GetDistanceFrom(begin);
}
void LdlbLabelHeader::Print(std::ostream& out) const {
    out << sender << " destinations=" << destinations.size();
}

void LdlbState::Sample(uint32_t port, uint32_t priority, uint64_t bytes,
                       uint64_t kmin, uint64_t kmax, double pmax, double random) {
    const auto& cfg = KvRoutingConfig::Get();
    auto& d = m_samples[{port, priority}];
    if (!cfg.ldlbGuard) {
        if (bytes > kmax) m_local.insert(port);
        else if (bytes <= kmin) m_local.erase(port);
        else if (random < pmax * double(bytes - kmin) / double(kmax - kmin))
            m_local.insert(port);
        return;
    }
    d.ewma = (1 - cfg.ldlbAlpha) * d.ewma + cfg.ldlbAlpha * double(bytes);
    if (!Local(port)) {
        const bool trigger = d.ewma > kmax ||
            (d.ewma > kmin && random < pmax * (d.ewma - kmin) / double(kmax - kmin));
        d.enter = trigger ? d.enter + 1 : 0;
        d.exit = 0;
        if (d.enter >= cfg.ldlbEnter) { m_local.insert(port); d.enter = 0; }
    } else if (d.ewma <= kmin) {
        ++d.exit; d.enter = 0;
        if (d.exit >= cfg.ldlbExit) { m_local.erase(port); d.exit = 0; }
    } else d.exit = 0;
}
bool LdlbState::Remote(uint32_t port, uint32_t dip) const {
    auto found = m_remote.find(port);
    return found != m_remote.end() && found->second.count(dip);
}
void LdlbState::Receive(uint32_t port, const std::set<uint32_t>& destinations) {
    m_remote[port] = destinations;
}
bool LdlbState::PollBroadcast(uint64_t now) {
    const auto& cfg = KvRoutingConfig::Get();
    if (m_pending != m_local) { m_pending = m_local; m_rounds = 1; }
    else if (m_rounds != std::numeric_limits<uint64_t>::max()) ++m_rounds;
    if (m_pending == m_advertised ||
        (cfg.ldlbDampening && (m_rounds < cfg.ldlbRounds ||
         (m_lastBroadcast && now - m_lastBroadcast < cfg.ldlbGapNs)))) return false;
    m_advertised = m_pending; m_lastBroadcast = now;
    return true;
}
KvDecision LdlbPolicy::Select(const KvRoutingContext& c) const {
    std::vector<const KvPortView*> candidates;
    for (const auto& p : c.ports)
        if (!p.localCongested && !p.remoteCongested) candidates.push_back(&p);
    const bool fallback = candidates.empty();
    if (fallback) for (const auto& p : c.ports) candidates.push_back(&p);
    uint64_t minimum = std::numeric_limits<uint64_t>::max();
    std::vector<int> ties;
    for (const auto* p : candidates) {
        if (!p->up) continue;
        if (p->queue < minimum) { minimum = p->queue; ties.clear(); }
        if (p->queue == minimum) ties.push_back(p->port);
    }
    if (ties.empty()) {
        for (const auto& p : c.ports) if (p.up) return {p.port, true, 0};
        return {};
    }
    return {ties[c.ldlbTieHash % ties.size()], fallback, 0};
}
}
