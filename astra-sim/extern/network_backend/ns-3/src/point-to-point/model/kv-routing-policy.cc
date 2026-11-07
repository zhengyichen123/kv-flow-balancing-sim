#include "kv-routing-policy.h"
#include "ldlb-routing.h"
#include "ns3/abort.h"
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ns3 {
namespace {
std::map<std::string, KvRoutingPolicy::Factory>& Registry() {
    static std::map<std::string, KvRoutingPolicy::Factory> registry;
    return registry;
}
class EcmpPolicy final : public KvRoutingPolicy {
public:
    explicit EcmpPolicy(KvTransport transport) : m_transport(transport) {}
    KvTransport Transport() const override { return m_transport; }
    KvDecision Select(const KvRoutingContext& c) const override {
        return {c.ecmpPort, false, 0};
    }
private:
    KvTransport m_transport;
};
class PressurePolicy final : public KvRoutingPolicy {
public:
    explicit PressurePolicy(bool feedback) : m_feedback(feedback) {}
    KvTransport Transport() const override { return KvTransport::BLOCK; }
    bool UsesFeedback() const override { return m_feedback; }
    KvDecision Select(const KvRoutingContext& c) const override {
        if (c.ports.empty()) return {};
        const auto& cfg = KvRoutingConfig::Get();
        const bool fresh = std::all_of(c.ports.begin(), c.ports.end(),
                                      [](const auto& p) { return p.fresh; });
        std::vector<std::pair<int, uint64_t>> costs;
        uint64_t minimum = std::numeric_limits<uint64_t>::max();
        for (const auto& p : c.ports) {
            uint64_t pressure = ServiceNs(p.queue + p.serial + p.reserved, p.bandwidth);
            if (m_feedback && fresh) {
                uint64_t aged = std::max(p.floor, p.horizon > p.age ? p.horizon - p.age : 0);
                pressure = std::max(pressure, aged);
            }
            const uint64_t cost = pressure + ServiceNs(c.wireBytes, p.bandwidth) + p.propagation;
            costs.emplace_back(p.port, cost);
            minimum = std::min(minimum, cost);
        }
        std::vector<std::pair<int, uint64_t>> ties;
        for (const auto& p : costs)
            if (p.second - minimum <= cfg.tieNs) ties.push_back(p);
        std::sort(ties.begin(), ties.end());
        auto chosen = ties[Hash(c) % ties.size()];
        return {chosen.first, m_feedback && !fresh, chosen.second};
    }
private:
    bool m_feedback;
};
void EnsureBuiltins() {
    static const bool initialized = [] {
        Registry().emplace("ECMP", [] { return std::make_unique<EcmpPolicy>(KvTransport::LEGACY); });
        Registry().emplace("MEGA_ECMP", [] { return std::make_unique<EcmpPolicy>(KvTransport::REQUEST); });
        Registry().emplace("BLOCK_ECMP", [] { return std::make_unique<EcmpPolicy>(KvTransport::BLOCK); });
        Registry().emplace("BLOCK_LOCAL", [] { return std::make_unique<PressurePolicy>(false); });
        Registry().emplace("BRBF_V1", [] { return std::make_unique<PressurePolicy>(true); });
        Registry().emplace("MEGA_LDLB", [] { return std::make_unique<LdlbPolicy>(); });
        return true;
    }();
    (void)initialized;
}
}

KvRoutingConfig& KvRoutingConfig::Get() { static KvRoutingConfig cfg; return cfg; }
bool KvRoutingConfig::Read(const std::string& key, std::istream& input) {
    if (key == "KV_LDLB_EWMA_ALPHA") {
        input >> ldlbAlpha;
        NS_ABORT_MSG_IF(!input, "Invalid " << key);
        return true;
    }
    if (key == "KV_ROUTING_MODE") {
        input >> mode;
        NS_ABORT_MSG_IF(!input, "Missing KV routing mode");
        return true;
    }
    uint64_t* value = nullptr;
    if (key == "KV_LOAD_REFRESH_NS") value = &refreshNs;
    else if (key == "KV_LOAD_MIN_GAP_NS") value = &minGapNs;
    else if (key == "KV_LOAD_TTL_NS") value = &ttlNs;
    else if (key == "KV_ROUTE_TIE_NS") value = &tieNs;
    else if (key == "KV_CONTROL_BUDGET_PPM") value = &budgetPpm;
    else if (key == "KV_BLOCK_TOMBSTONE_NS") value = &tombstoneNs;
    else if (key == "KV_PARENT_WARN_NS") value = &warnNs;
    else if (key == "KV_PARENT_TIMEOUT_NS") value = &timeoutNs;
    else if (key == "KV_ROUTE_SEED") value = &seed;
    else if (key == "KV_LDLB_MIN_BROADCAST_NS") value = &ldlbGapNs;
    if (value) { input >> *value; NS_ABORT_MSG_IF(!input, "Invalid " << key); return true; }
    uint32_t* small = nullptr;
    if (key == "KV_BLOCK_BYTES") small = &blockBytes;
    else if (key == "KV_PARENT_BLOCK_WINDOW") small = &parentWindow;
    else if (key == "KV_HOST_ACTIVE_BLOCKS") small = &hostWindow;
    else if (key == "KV_LDLB_STABILITY_GUARD") small = &ldlbGuard;
    else if (key == "KV_LDLB_ENTER_SAMPLES") small = &ldlbEnter;
    else if (key == "KV_LDLB_EXIT_SAMPLES") small = &ldlbExit;
    else if (key == "KV_LDLB_BROADCAST_DAMPENING") small = &ldlbDampening;
    else if (key == "KV_LDLB_BROADCAST_STABLE_ROUNDS") small = &ldlbRounds;
    if (small) { input >> *small; NS_ABORT_MSG_IF(!input, "Invalid " << key); return true; }
    NS_ABORT_MSG_IF(key.rfind("KV_", 0) == 0, "Unknown KV configuration: " << key);
    return false;
}
void KvRoutingConfig::Validate() const {
    NS_ABORT_MSG_IF(ldlbGuard > 1 || ldlbDampening > 1 || !ldlbEnter || !ldlbExit ||
                    !ldlbRounds || !std::isfinite(ldlbAlpha) || ldlbAlpha <= 0 ||
                    ldlbAlpha > 1, "Invalid LDLB configuration");
    auto policy = KvRoutingPolicy::Create(mode);
    if (policy->Transport() == KvTransport::LEGACY) return;
    NS_ABORT_MSG_IF(blockBytes != 2097152 || mtu != 4000 || !parentWindow ||
                    parentWindow > 64 || hostWindow > 1024 ||
                    hostWindow < parentWindow || hostWindow % parentWindow ||
                    !refreshNs || !minGapNs || minGapNs > refreshNs ||
                    ttlNs < refreshNs || !budgetPpm || budgetPpm > 1000000 ||
                    !tombstoneNs || !warnNs || timeoutNs <= warnNs,
                    "Invalid KV routing configuration");
}
void KvRoutingPolicy::Register(const std::string& name, Factory factory) {
    EnsureBuiltins();
    if (!factory || !Registry().emplace(name, std::move(factory)).second)
        throw std::invalid_argument("Duplicate/invalid KV policy: " + name);
}
std::unique_ptr<KvRoutingPolicy> KvRoutingPolicy::Create(const std::string& name) {
    EnsureBuiltins();
    auto found = Registry().find(name);
    if (found == Registry().end()) throw std::invalid_argument("Unknown KV policy: " + name);
    return found->second();
}
std::vector<std::string> KvRoutingPolicy::Names() {
    EnsureBuiltins();
    std::vector<std::string> out;
    for (const auto& item : Registry()) out.push_back(item.first);
    return out;
}
uint64_t KvRoutingPolicy::ServiceNs(uint64_t bytes, uint64_t rate) {
    NS_ABORT_MSG_IF(!rate, "Zero link bandwidth");
    const auto numerator = static_cast<unsigned __int128>(bytes) * 8000000000ULL;
    return static_cast<uint64_t>((numerator + rate - 1) / rate);
}
uint64_t KvRoutingPolicy::Hash(const KvRoutingContext& c) {
    uint64_t h = 14695981039346656037ULL;
    const auto feed = [&](uint64_t value, unsigned bytes) {
        for (unsigned n = bytes; n; --n) {
            h ^= (value >> (8 * (n - 1))) & 255;
            h *= 1099511628211ULL;
        }
    };
    feed(KvRoutingConfig::Get().seed, 8); feed(c.switchId, 4);
    feed(c.transfer, 8); feed(c.request, 8); feed(c.epoch, 4); feed(c.block, 4);
    return h;
}
KvLoadRecord KvRoutingPolicy::Advertise(const KvRoutingContext& c) const {
    KvLoadRecord record;
    record.destination = c.destination;
    record.oldest = c.now;
    const bool fresh = std::all_of(c.ports.begin(), c.ports.end(),
                                  [](const auto& p) { return p.fresh; });
    uint64_t minimum = std::numeric_limits<uint64_t>::max();
    for (const auto& p : c.ports) {
        auto h = ServiceNs(p.queue + p.serial + p.reserved, p.bandwidth);
        auto floor = ServiceNs(p.reserved, p.bandwidth);
        auto oldest = c.now;
        if (fresh && !p.terminal) {
            h = std::max(h, std::max(p.floor, p.horizon > p.age ? p.horizon - p.age : 0));
            floor = std::max(floor, p.floor);
            oldest = std::min(oldest, p.oldest);
        }
        if (h < minimum) {
            minimum = h; record.horizon = h; record.floor = floor; record.oldest = oldest;
        }
    }
    record.flags = fresh && !c.ports.empty() ? KvLoadRecord::FRESH : 0;
    return record;
}

KvRoutingStats& KvRoutingStats::Get() { static KvRoutingStats stats; return stats; }
void KvRoutingStats::Open(const KvRoutingConfig& cfg) {
    m_rx.clear();
    for (auto* stream : {&blocks, &parents, &decisions, &feedback, &ports})
        if (stream->is_open()) stream->close();
    if (cfg.mode == "ECMP") return;
    std::filesystem::create_directories(cfg.output);
    auto open = [&](std::ofstream& stream, const char* name, const char* header) {
        if (stream.is_open()) stream.close();
        stream.open(std::filesystem::path(cfg.output) / name);
        NS_ABORT_MSG_IF(!stream, "Cannot open KV statistics: " << name);
        stream << header << '\n';
    };
    open(blocks, "block_fct.csv", "request_id,transfer_id,block,valid_bytes,payload_bytes,ready_ns,first_tx_ns,rx_complete_ns,last_ack_ns");
    open(parents, "kv_transfers.csv", "request_id,transfer_id,valid_bytes,payload_bytes,ready_ns,first_tx_ns,rx_complete_ns,last_ack_ns,kv_net_fct_ns,kv_ready_delay_ns,mode");
    open(decisions, "route_decisions.csv", "time_ns,switch_id,request_id,transfer_id,block,chosen_port,mode,fallback,cost_ns,candidate_port,Q,S,R,H,F,age_ns,fresh,bandwidth_bps,propagation_ns,local_congested,remote_congested");
    open(feedback, "feedback_stats.csv", "time_ns,switch_id,port,event,sequence,wire_bytes,records");
    open(ports, "port_stats.csv", "time_ns,switch_id,port,tx_bytes,Q,S,R");
    std::ofstream manifest(std::filesystem::path(cfg.output) / "kv_manifest.json");
    manifest << "{\"mode\":\"" << cfg.mode << "\",\"block_bytes\":" << cfg.blockBytes
             << ",\"parent_window\":" << cfg.parentWindow << ",\"host_window\":" << cfg.hostWindow
             << ",\"refresh_ns\":" << cfg.refreshNs << ",\"ttl_ns\":" << cfg.ttlNs
             << ",\"seed\":" << cfg.seed << ",\"kv_header_bytes\":72"
             << ",\"ldlb_reference_commit\":\"39bcd806a476f37db2667ac894fd084a96c69ed6\""
             << ",\"ldlb_stability_guard\":" << cfg.ldlbGuard
             << ",\"ldlb_ewma_alpha\":" << cfg.ldlbAlpha
             << ",\"ldlb_enter_samples\":" << cfg.ldlbEnter
             << ",\"ldlb_exit_samples\":" << cfg.ldlbExit
             << ",\"ldlb_broadcast_dampening\":" << cfg.ldlbDampening
             << ",\"ldlb_stable_rounds\":" << cfg.ldlbRounds
             << ",\"ldlb_min_broadcast_ns\":" << cfg.ldlbGapNs << "}\n";
}
void KvRoutingStats::RecordRx(uint64_t transfer, uint32_t block, uint64_t now) {
    m_rx.emplace(std::make_pair(transfer, block), now);
}
uint64_t KvRoutingStats::RxTime(uint64_t transfer, uint32_t block) const {
    auto found = m_rx.find({transfer, block});
    return found == m_rx.end() ? 0 : found->second;
}
void KvRoutingStats::Forget(uint64_t transfer) {
    auto first = m_rx.lower_bound({transfer, 0});
    while (first != m_rx.end() && first->first.first == transfer) first = m_rx.erase(first);
}
}
