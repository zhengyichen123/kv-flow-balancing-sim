#include "../../../scratch/common.h"
#include "ns3/ppp-header.h"
#include <set>
#include <stdexcept>

namespace {
constexpr uint32_t requests = 12;
constexpr uint64_t validBytes = 65 * 131072ULL;
uint32_t callbacks = 0;
std::set<uint64_t> completed;

void Notify() {
    if (++callbacks == requests) Simulator::Stop(MicroSeconds(2100));
}

void Sent() {}

void SendLabel(bool relief, bool forged = false) {
    auto spine = DynamicCast<SwitchNode>(n.Get(18));
    const auto port = nbr2if[n.Get(18)][n.Get(16)].idx;
    auto device = DynamicCast<QbbNetDevice>(spine->GetDevice(port));
    LdlbLabelHeader label; label.sender = forged ? 999 : spine->GetId();
    if (!relief) label.destinations.insert(serverAddress[8].Get());
    auto packet = Create<Packet>(); packet->AddHeader(label);
    Ipv4Header ip; ip.SetProtocol(LdlbLabelHeader::PROTOCOL); ip.SetTtl(1);
    ip.SetPayloadSize(packet->GetSize()); packet->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); packet->AddHeader(ppp);
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header);
    packet->PeekHeader(ch);
    device->SwitchSend(0, packet, ch);
}

void OnComplete(FILE*, Ptr<RdmaQueuePair> qp) {
    NS_ABORT_MSG_IF(!qp->kvCompletedParent || qp->m_size != validBytes ||
                    !completed.insert(qp->m_tag).second,
                    "Invalid or repeated parent completion");
}

void Issue() {
    auto driver = n.Get(0)->GetObject<RdmaDriver>();
    for (uint32_t r = 0; r < requests; ++r)
        driver->AddQueuePair(0, 8, (1 << 20) + r, validBytes, 3,
                            serverAddress[0], serverAddress[8], 10000 + r, 100,
                            pairBdp[n.Get(0)][n.Get(8)], pairRtt[0][8],
                            MakeCallback(&Notify), MakeCallback(&Sent));
    std::set<uint64_t> active;
    for (const auto& item : driver->m_rdma->m_qpMap) {
        NS_ABORT_MSG_IF(!item.second->kvParent, "Child lacks shared parent");
        active.insert(item.second->kvParent->id);
    }
    NS_ABORT_MSG_IF(active.size() != 8, "Initial active parent quota was not eight");
}
}

int main(int argc, char** argv) {
    if (argc != 2 || !ReadConf(argv[1])) return 1;
    SetConfig();
    if (!SetupNetwork(&OnComplete)) return 1;
    // 同一时刻发行多个父传输，不受 serving 图中串行 SEND 的限制。
    auto policy = KvRoutingPolicy::Create(KvRoutingConfig::Get().mode);
    if (policy->UsesLabels()) {
        // 在现有 2-leaf/4-spine 测试拓扑上通过真实队列/链路注入标签、解除和伪造邻居。
        NS_ABORT_MSG_IF(n.GetN() != 22 || !nbr2if[n.Get(18)].count(n.Get(16)),
                        "LDLB smoke requires the 16-host 2-leaf/4-spine topology");
        Simulator::ScheduleNow(&SendLabel, false, false);
        Simulator::Schedule(MicroSeconds(10), &Issue);
        Simulator::Schedule(MicroSeconds(20), &SendLabel, true, false);
        Simulator::Schedule(MicroSeconds(30), &SendLabel, false, true);
    } else Simulator::ScheduleNow(&Issue);
    Simulator::Stop(MilliSeconds(100));
    Simulator::Run();
    NS_ABORT_MSG_IF(callbacks != requests || completed.size() != requests,
                    "Queued parent transfers failed to complete");
    std::cout << "PASS concurrent parent quota and exactly-once completion\n";
    Simulator::Destroy();
}
