#include "ns3/kv-routing-policy.h"
#include "ns3/ldlb-routing.h"
#include "ns3/kv-routing-header.h"
#include "ns3/rdma-queue-pair.h"
#include "ns3/packet.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"
#include "ns3/seq-ts-header.h"
#include "ns3/ppp-header.h"
#include "ns3/qbb-header.h"
#include "ns3/simulator.h"
#include <iostream>
#include <stdexcept>

using namespace ns3;

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
class RegisteredPolicy : public KvRoutingPolicy {
public:
    KvTransport Transport() const override { return KvTransport::BLOCK; }
    KvDecision Select(const KvRoutingContext& c) const override {
        return {c.ports.back().port, false, 7};
    }
};
void RunChecks() {
    auto saved = KvRoutingConfig::Get();
    auto& cfg = KvRoutingConfig::Get();
    cfg = KvRoutingConfig{};
    {
        auto mega = KvRoutingPolicy::Create("MEGA_LDLB");
        Check(mega->Transport() == KvTransport::REQUEST &&
              mega->UsesLabels() && !mega->UsesFeedback(), "LDLB transport/feedback contract failed");
        LdlbLabelHeader label; label.sender = 18;
        label.destinations = {0x0a000000, 0x0a000001, 0x0a000003, 0x0a0000ff,
                              0x0a000100, 0x0a0001ff};
        auto packet = Create<Packet>(); packet->AddHeader(label);
        const std::vector<uint8_t> expected = {
            0,0,0,18, 0,0,0,2, 10,0,0,0, 0,0,0,3, 0,1,3,3,255,255,
            10,0,1,0, 0,0,0,2, 0,0,255,255};
        std::vector<uint8_t> encoded(packet->GetSize());
        packet->CopyData(encoded.data(), encoded.size());
        Check(encoded == expected, "LDLB codec differs from author /24 range format");
        LdlbLabelHeader decoded;
        Check(packet->PeekHeader(decoded) == expected.size() && decoded.valid &&
              decoded.destinations == label.destinations, "LDLB label roundtrip failed");
        for (size_t length = 0; length < expected.size(); ++length) {
            auto truncated = Create<Packet>(expected.data(), length);
            Check(truncated->PeekHeader(decoded) == 0 && !decoded.valid &&
                  decoded.destinations.empty(), "Partial LDLB snapshot accepted");
        }
        auto invalid = expected; invalid[16] = 2; invalid[17] = 1;
        Check(Create<Packet>(invalid.data(), invalid.size())->PeekHeader(decoded) == 0 &&
              !decoded.valid, "Reversed LDLB suffix range accepted");
        label.destinations.clear(); packet = Create<Packet>(); packet->AddHeader(label);
        Check(packet->PeekHeader(decoded) == 8 && decoded.valid && decoded.destinations.empty(),
              "Empty LDLB relief snapshot failed");
        cfg.ldlbAlpha = 1;
        LdlbState state;
        state.Sample(9, 3, 201, 100, 200, 1, .5);
        Check(!state.Local(9), "LDLB entered before two consecutive samples");
        state.Sample(9, 3, 201, 100, 200, 1, .5);
        Check(state.Local(9), "LDLB failed high-threshold entry");
        Check(!state.PollBroadcast(100) && state.PollBroadcast(101) &&
              !state.PollBroadcast(102), "LDLB broadcast stability/change guard failed");
        state.Receive(10, {123});
        Check(state.Remote(10, 123) && !state.Remote(9, 123) &&
              !state.Remote(10, 124), "LDLB neighbor/destination isolation failed");
        state.Receive(10, {});
        Check(!state.Remote(10, 123), "LDLB relief did not replace CLT snapshot");
        state.Sample(9, 3, 100, 100, 200, 1, .5);
        state.Sample(9, 3, 100, 100, 200, 1, .5);
        Check(state.Local(9), "LDLB cleared before three samples");
        state.Sample(9, 3, 100, 100, 200, 1, .5);
        Check(!state.Local(9) && !state.PollBroadcast(102) &&
              !state.PollBroadcast(200) && state.PollBroadcast(50101),
              "LDLB exit/minimum broadcast interval failed");
        // 概率失败会中断连续进入；回差区不会清除已进入的标签。
        state.Sample(9, 3, 150, 100, 200, 1, .4);
        state.Sample(9, 3, 150, 100, 200, 1, .6);
        state.Sample(9, 3, 150, 100, 200, 1, .4);
        Check(!state.Local(9), "LDLB probability misses did not reset entry");
        state.Sample(9, 3, 150, 100, 200, 1, .4);
        state.Sample(9, 3, 150, 100, 200, 1, .99);
        Check(state.Local(9), "LDLB middle-range entry/hysteresis failed");
        cfg.ldlbGuard = 0;
        state.Sample(9, 3, 100, 100, 200, 1, .9);
        Check(!state.Local(9), "LDLB raw detector low threshold failed");
        cfg = KvRoutingConfig{};
        LdlbState smooth;
        smooth.Sample(9, 3, 400, 100, 200, 0, .5); // EWMA=100
        smooth.Sample(9, 3, 400, 100, 200, 0, .5); // EWMA=175
        smooth.Sample(9, 3, 400, 100, 200, 0, .5); // EWMA=231.25
        Check(!smooth.Local(9), "LDLB EWMA initialized from first sample");
        smooth.Sample(9, 3, 400, 100, 200, 0, .5);
        Check(smooth.Local(9), "LDLB default EWMA entry failed");
        KvRoutingContext choices;
        for (int port = 9; port <= 11; ++port) {
            KvPortView p; p.port = port; p.queue = port == 11 ? 20 : 10;
            p.reserved = port == 10 ? 10000000 : 0; choices.ports.push_back(p);
        }
        choices.ports[0].localCongested = true;
        Check(mega->Select(choices).port == 10, "LDLB used reservations or ignored LCT");
        choices.ports[1].remoteCongested = true;
        Check(mega->Select(choices).port == 11, "LDLB ignored CLT filter union");
        choices.ports[2].localCongested = true; choices.ldlbTieHash = 1;
        auto decision = mega->Select(choices);
        Check(decision.fallback && decision.port == 10, "LDLB all-blocked fallback/tie failed");
        choices.ports[0].up = choices.ports[1].up = false;
        Check(mega->Select(choices).port == 11, "LDLB chose a down port");
    }
    Check(KvRoutingPolicy::Create("ECMP")->Transport() == KvTransport::LEGACY,
          "plain ECMP transport changed");
    KvRoutingContext c;
    c.ecmpPort = 9; c.switchId = 16; c.transfer = 17; c.request = 3;
    c.now = 100000; c.wireBytes = 2153852;
    for (int port = 9; port < 13; ++port) {
        KvPortView p; p.port = port; p.bandwidth = 100000000000ULL;
        p.fresh = true; p.oldest = c.now;
        c.ports.push_back(p);
    }
    c.ports[0].reserved = cfg.blockBytes;
    Check(KvRoutingPolicy::Create("ECMP")->Select(c).port == 9,
          "ECMP unexpectedly became load aware");
    auto local = KvRoutingPolicy::Create("BLOCK_LOCAL");
    Check(local->Select(c).port != 9, "local reservation did not affect selection");
    c.ports[0].reserved = 0; c.ports[0].horizon = 500000;
    auto brbf = KvRoutingPolicy::Create("BRBF_V1");
    Check(brbf->Select(c).port != 9 && !brbf->Select(c).fallback,
          "downstream bottleneck was ignored");
    c.ports[1].fresh = false;
    Check(brbf->Select(c).fallback, "partial stale feedback did not trigger uniform fallback");
    c.ports[1].fresh = true;
    for (auto& p : c.ports) { p.horizon = 40000; p.floor = 30000; p.age = 20000; }
    auto record = brbf->Advertise(c);
    Check(record.horizon == 30000 && record.floor == 30000,
          "unarrived reservation floor decayed");
    KvRoutingPolicy::Register("UNIT_TEST_POLICY", [] { return std::make_unique<RegisteredPolicy>(); });
    Check(KvRoutingPolicy::Create("UNIT_TEST_POLICY")->Select(c).port == 12,
          "custom registered policy was not invoked");
    bool rejected = false;
    try { KvRoutingPolicy::Create("NOT_A_POLICY"); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "unknown policy silently accepted");
    rejected = false;
    try { KvRoutingPolicy::Register("ECMP", [] { return std::make_unique<RegisteredPolicy>(); }); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "registered plugin replaced the basic ECMP");
    Check(KvRoutingPolicy::ServiceNs(2153852, 100000000000ULL) == 172309,
          "wire service time did not round upward");

    // 相同负载下比较不同速率；相同速率下比较每个候选路径的传播延迟。
    KvRoutingContext varied;
    varied.wireBytes = c.wireBytes;
    KvPortView slow, fast;
    slow.port = 1; slow.bandwidth = 100000000000ULL; slow.fresh = true;
    fast.port = 2; fast.bandwidth = 400000000000ULL; fast.fresh = true;
    varied.ports = {slow, fast};
    Check(local->Select(varied).port == 2 && brbf->Select(varied).port == 2,
          "heterogeneous bandwidth was ignored");
    varied.ports[0].bandwidth = fast.bandwidth;
    varied.ports[1].propagation = 20000;
    Check(local->Select(varied).port == 1 && brbf->Select(varied).port == 1,
          "per-port propagation delay was ignored");
    Check(local->Select(varied).cost == KvRoutingPolicy::ServiceNs(
              varied.wireBytes, fast.bandwidth),
          "propagation leaked across candidate ports");
    varied.ports[0].propagation = 40000;
    Check(local->Select(varied).port == 2, "lower-delay port was not selected");
    varied.ports[0].bandwidth = slow.bandwidth;
    varied.ports[0].propagation = 0;
    Check(local->Select(varied).port == 2, "bandwidth and delay were not combined");

    auto control = CreateObject<RdmaQueuePair>(3, Ipv4Address(1), Ipv4Address(2), 10000, 100);
    control->m_rate = control->m_max_rate = DataRate("100Gbps");
    control->m_win = 116000; control->m_var_win = true;
    auto parent = Create<RdmaKvTransfer>(); parent->control = control;
    auto child = CreateObject<RdmaQueuePair>(3, Ipv4Address(1), Ipv4Address(2), 49152, 49500);
    child->kvParent = parent;
    parent->inFlight = 116000;
    Check(child->IsWinBound(), "child obtained an independent congestion window");
    control->m_rate = DataRate("10Gbps");
    Check(child->GetWin() == 11600, "child ignored parent rate-scaled window");

    KvBlockHeader header;
    header.transfer = 0x1020304050607080ULL;
    header.request = 3; header.parentBytes = uint64_t{1} << 33;
    header.parentBlocks = header.blocks = 4096; header.block = 2;
    header.offset = 4000; header.validBytes = 2097152; header.cookie = 20;
    auto packet = Create<Packet>(4000); packet->AddHeader(header);
    SeqTsHeader seq; seq.SetSeq(4000); seq.SetPG(3); packet->AddHeader(seq);
    UdpHeader udp; udp.SetSourcePort(49152); udp.SetDestinationPort(49500); packet->AddHeader(udp);
    Ipv4Header ip; ip.SetProtocol(0x11); ip.SetSource(Ipv4Address(1)); ip.SetDestination(Ipv4Address(2));
    ip.SetPayloadSize(packet->GetSize()); packet->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); packet->AddHeader(ppp);
    CustomHeader parsed(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    packet->PeekHeader(parsed);
    Check(parsed.kvPresent && parsed.kv.transfer == header.transfer &&
          parsed.kv.parentBytes == header.parentBytes &&
          packet->GetSize() - parsed.GetSerializedSize() == 4000,
          "KV header truncation or payload accounting error");

    auto legacy = Create<Packet>(4000);
    legacy->AddHeader(seq); udp.SetDestinationPort(100); legacy->AddHeader(udp);
    ip.SetPayloadSize(legacy->GetSize()); legacy->AddHeader(ip); legacy->AddHeader(ppp);
    legacy->PeekHeader(parsed);
    Check(!parsed.kvPresent && legacy->GetSize() - parsed.GetSerializedSize() == 4000,
          "legacy ECMP data acquired KV overhead");
    auto rebuilt = Create<Packet>(4000); rebuilt->AddHeader(parsed);
    CustomHeader reparsed(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    rebuilt->PeekHeader(reparsed);
    Check(rebuilt->GetSize() == legacy->GetSize() && reparsed.udp.seq == 4000 &&
          reparsed.udp.dport == 100 && !reparsed.kvPresent,
          "shared header serialization broke legacy ECMP");

    auto ack = Create<Packet>();
    header.flags |= KvBlockHeader::ACK; ack->AddHeader(header);
    qbbHeader ackHeader; ackHeader.SetSport(49500); ackHeader.SetDport(49152);
    ackHeader.SetSeq(2097152); ackHeader.SetPG(3); ack->AddHeader(ackHeader);
    ip.SetProtocol(0xFC); ip.SetPayloadSize(ack->GetSize());
    ack->AddHeader(ip); ack->AddHeader(ppp);
    ack->PeekHeader(parsed);
    Check(parsed.kvPresent && parsed.kv.cookie == 20 && parsed.ack.seq == 2097152,
          "ACK lost the reverse route cookie or completion sequence");
    rebuilt = Create<Packet>(); rebuilt->AddHeader(parsed); rebuilt->PeekHeader(reparsed);
    Check(rebuilt->GetSize() == ack->GetSize() && reparsed.kvPresent &&
          reparsed.kv.transfer == header.transfer && reparsed.ack.seq == 2097152,
          "shared ACK header serialization corrupted KV metadata");

    KvLoadHeader load;
    load.sender = 18; load.sequence = 1; load.generated = 10000;
    load.records.push_back({2, 100000, 20000, 10000, KvLoadRecord::FRESH});
    auto feedback = Create<Packet>(); feedback->AddHeader(load);
    Check(feedback->GetSize() == 76, "feedback did not occupy real bytes");
    KvLoadHeader decoded;
    Check(feedback->PeekHeader(decoded) == 76 && decoded.valid &&
          decoded.records[0].floor == 20000, "feedback roundtrip failed");
    std::vector<uint8_t> bytes(feedback->GetSize());
    feedback->CopyData(bytes.data(), bytes.size()); bytes.back() ^= 1;
    auto corrupt = Create<Packet>(bytes.data(), bytes.size());
    Check(corrupt->PeekHeader(decoded) == 0 && !decoded.valid,
          "feedback CRC corruption was accepted");
    cfg = saved;
}
}

#ifdef KV_ROUTING_STANDALONE
int main() {
    try { RunChecks(); std::cout << "PASS KV policies, LDLB labels/detector, shared window, headers, CRC and plugins\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
#else
#include "ns3/test.h"
class KvRoutingCase : public TestCase {
public:
    KvRoutingCase() : TestCase("KV routing protocol and strategy contracts") {}
    void DoRun() override {
        std::string error;
        try { RunChecks(); } catch (const std::exception& e) { error = e.what(); }
        NS_TEST_ASSERT_MSG_EQ(error, "", "KV routing contract failed");
    }
};
class KvRoutingSuite : public TestSuite {
public:
    KvRoutingSuite() : TestSuite("kv-routing", UNIT) {
        AddTestCase(new KvRoutingCase(), TestCase::Duration::QUICK);
    }
} kvRoutingSuite;
#endif
