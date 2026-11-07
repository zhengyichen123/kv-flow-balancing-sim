#include "rdma-hw.h"
#include "kv-routing-policy.h"
#include "ns3/abort.h"
#include "ns3/simulator.h"
#include <algorithm>
#include <iostream>
#include <limits>

namespace ns3 {
void RdmaHw::AddKvParent(Ptr<RdmaQueuePair> control) {
    const auto& cfg = KvRoutingConfig::Get();
    auto policy = KvRoutingPolicy::Create(cfg.mode);
    NS_ABORT_MSG_IF(m_cc_mode != 1, "KV block transport requires DCQCN (CC_MODE 1)");
    auto parent = Create<RdmaKvTransfer>();
    parent->control = control;
    parent->validBytes = control->m_size;
    parent->blocks = (parent->validBytes + cfg.blockBytes - 1) / cfg.blockBytes;
    parent->payloadBytes = uint64_t(parent->blocks) * cfg.blockBytes;
    NS_ABORT_MSG_IF(!parent->validBytes ||
                    parent->payloadBytes > std::numeric_limits<uint32_t>::max(),
                    "KV parent padded size exceeds the common 32-bit transport limit");
    parent->request = control->m_tag - (1 << 20);
    NS_ABORT_MSG_IF(++m_kvCounter >= (uint64_t{1} << 48) || control->m_src > 65535,
                    "KV transfer ID exhausted");
    parent->id = (uint64_t(control->m_src) << 48) | m_kvCounter;
    parent->ready = parent->lastProgress = Simulator::Now().GetNanoSeconds();
    parent->mega = policy->Transport() == KvTransport::REQUEST;
    control->dport = KvBlockHeader::UDP_PORT;
    const auto nic = GetNicIdxOfQp(control);
    control->m_rate = control->m_max_rate = m_nic[nic].dev->GetDataRate();
    control->mlx.m_targetRate = control->m_rate;
    parent->nextAvail = Simulator::Now();
    m_kvParents.emplace(parent->id, parent);
    parent->watchdog = Simulator::Schedule(NanoSeconds(cfg.warnNs),
                                           &RdmaHw::CheckKvProgress, this, parent);
    FillKvWindow();
}

uint16_t RdmaHw::AllocateKvPort(uint32_t dip, uint16_t pg) {
    auto& cursor = m_kvPortCursor[dip];
    const uint64_t now = Simulator::Now().GetNanoSeconds();
    for (unsigned n = 0; n < 16384; ++n) {
        cursor = cursor < 49152 || cursor == 65535 ? 49152 : cursor + 1;
        auto expiry = m_kvPortExpiry.find({dip, cursor});
        if (m_qpMap.count(GetQpKey(dip, cursor, pg)) ||
            (expiry != m_kvPortExpiry.end() && expiry->second > now)) continue;
        if (expiry != m_kvPortExpiry.end()) m_kvPortExpiry.erase(expiry);
        return cursor;
    }
    if (m_kvPortRetry.IsExpired())
        m_kvPortRetry = Simulator::Schedule(NanoSeconds(KvRoutingConfig::Get().tombstoneNs),
                                             &RdmaHw::FillKvWindow, this);
    return 0;
}

void RdmaHw::FillKvWindow() {
    const auto& cfg = KvRoutingConfig::Get();
    while (m_kvActive < cfg.hostWindow) {
        const auto activeParents = std::count_if(m_kvParents.begin(), m_kvParents.end(),
            [](const auto& item) { return item.second->active != 0; });
        auto chosen = m_kvParents.end();
        auto chooseRange = [&](auto begin, auto end) {
            for (auto it = begin; it != end; ++it) {
                auto p = it->second;
                const auto count = p->mega ? 1u : p->blocks;
                if (!p->done && p->nextBlock < count &&
                    (p->active || activeParents < cfg.hostWindow / cfg.parentWindow) &&
                    p->active < (p->mega ? 1u : cfg.parentWindow)) {
                    chosen = it; return;
                }
            }
        };
        chooseRange(m_kvParents.upper_bound(m_kvFillCursor), m_kvParents.end());
        if (chosen == m_kvParents.end())
            chooseRange(m_kvParents.begin(), m_kvParents.upper_bound(m_kvFillCursor));
        if (chosen == m_kvParents.end()) break;
        auto p = chosen->second;
        auto c = p->control;
        const auto port = p->mega ? c->sport : AllocateKvPort(c->dip.Get(), c->m_pg);
        if (!port) break;
        const auto key = GetQpKey(c->dip.Get(), port, c->m_pg);
        NS_ABORT_MSG_IF(m_qpMap.count(key), "KV QP port collision");
        auto qp = CreateObject<RdmaQueuePair>(c->m_pg, c->sip, c->dip, port,
                                             KvBlockHeader::UDP_PORT);
        qp->SetSrc(c->m_src); qp->SetDest(c->m_dest); qp->SetTag(c->m_tag);
        qp->SetSize(p->mega ? p->payloadBytes : cfg.blockBytes);
        qp->SetInitialSize(qp->m_size); qp->SetWin(c->m_win);
        qp->SetBaseRtt(c->m_baseRtt); qp->SetVarWin(c->m_var_win);
        qp->kvParent = p;
        if (!p->active) p->lastProgress = Simulator::Now().GetNanoSeconds();
        qp->m_rate = c->m_rate; qp->m_max_rate = c->m_max_rate;
        qp->m_nextAvail = Simulator::Now();
        qp->kvMeta.flags = p->mega ? KvBlockHeader::MEGA : KvBlockHeader::BLOCK;
        qp->kvMeta.transfer = p->id; qp->kvMeta.request = p->request;
        qp->kvMeta.parentBytes = p->validBytes;
        qp->kvMeta.blocks = qp->kvMeta.parentBlocks = p->blocks;
        qp->kvMeta.block = p->nextBlock++;
        qp->kvMeta.tag = c->m_tag;
        const uint64_t remaining = p->validBytes - uint64_t(qp->kvMeta.block) * cfg.blockBytes;
        qp->kvMeta.validBytes = std::min<uint64_t>(cfg.blockBytes, remaining);
        ++p->active; ++m_kvActive; m_kvFillCursor = p->id;
        m_qpMap.emplace(key, qp);
        auto nic = GetNicIdxOfQp(qp);
        m_nic[nic].qpGrp->AddQp(qp);
        m_nic[nic].dev->NewQp(qp);
    }
}

void RdmaHw::CompleteKvBlock(Ptr<RdmaQueuePair> qp) {
    auto p = qp->kvParent;
    auto& stats = KvRoutingStats::Get();
    const auto& cfg = KvRoutingConfig::Get();
    const uint64_t now = Simulator::Now().GetNanoSeconds();
    if (!p->mega && stats.blocks) {
        stats.blocks << p->request << ',' << p->id << ',' << qp->kvMeta.block << ','
            << qp->kvMeta.validBytes << ',' << cfg.blockBytes << ',' << p->ready << ','
            << qp->kvFirstTx << ',' << stats.RxTime(p->id, qp->kvMeta.block) << ',' << now << '\n';
    }
    --p->active; --m_kvActive; ++p->completed;
    m_kvPortExpiry[{qp->dip.Get(), qp->sport}] = now + cfg.tombstoneNs;
    DeleteQueuePair(qp);
    if (p->completed == (p->mega ? 1u : p->blocks)) {
        p->done = true;
        Simulator::Cancel(p->watchdog);
        Simulator::Cancel(p->control->mlx.m_eventUpdateAlpha);
        Simulator::Cancel(p->control->mlx.m_eventDecreaseRate);
        Simulator::Cancel(p->control->mlx.m_rpTimer);
        const auto rx = stats.RxTime(p->id, std::numeric_limits<uint32_t>::max());
        NS_ABORT_MSG_IF(!rx || !p->sent, "Missing KV parent completion observations");
        if (stats.parents) {
            stats.parents << p->request << ',' << p->id << ',' << p->validBytes << ','
                << p->payloadBytes << ',' << p->ready << ',' << p->firstTx << ',' << rx << ','
                << now << ',' << rx - p->firstTx << ',' << rx - p->ready << ',' << cfg.mode << '\n';
            stats.parents.flush(); stats.blocks.flush();
        }
        auto proxy = p->control;
        proxy->kvCompletedParent = true;
        proxy->kvMeta.transfer = p->id;
        proxy->m_init_size = p->payloadBytes;
        proxy->startTime = NanoSeconds(p->ready);
        proxy->snd_nxt = proxy->snd_una = proxy->m_size;
        m_kvParents.erase(p->id);
        m_qpCompleteCallback(proxy);
        proxy->m_notifyAppFinish();
        stats.Forget(p->id);
    }
    FillKvWindow();
}

void RdmaHw::ObserveKvReceive(Ptr<RdmaRxQueuePair> rx, const CustomHeader& ch) {
    const auto& cfg = KvRoutingConfig::Get();
    const auto& h = ch.kv;
    const auto count = (h.parentBytes + cfg.blockBytes - 1) / cfg.blockBytes;
    NS_ABORT_MSG_IF(!h.parentBytes || h.parentBlocks != count || h.blocks != count ||
                    h.block >= count || h.offset >= cfg.blockBytes ||
                    h.validBytes != std::min<uint64_t>(cfg.blockBytes,
                        h.parentBytes - uint64_t(h.block) * cfg.blockBytes),
                    "Invalid KV transfer descriptor");
    if (rx->kvTransfer && rx->kvTransfer != h.transfer)
        NS_ABORT_MSG_IF(rx->ReceiverNextExpectedSeq != 0, "KV receiver identity collision");
    rx->kvTransfer = h.transfer;
    auto& parent = m_kvRxParents[h.transfer];
    if (!parent.blocks) { parent.blocks = count; parent.bytes = h.parentBytes; }
    NS_ABORT_MSG_IF(parent.blocks != count || parent.bytes != h.parentBytes,
                    "KV parent descriptor changed");
    const uint64_t now = Simulator::Now().GetNanoSeconds();
    parent.expiry = now + cfg.timeoutNs;
    const auto completed = h.flags & KvBlockHeader::MEGA
        ? rx->ReceiverNextExpectedSeq / cfg.blockBytes
        : rx->ReceiverNextExpectedSeq == cfg.blockBytes ? 1u : 0u;
    while (rx->kvCompletedBlocks < completed) {
        const auto block = h.flags & KvBlockHeader::MEGA
            ? rx->kvCompletedBlocks : h.block;
        ++rx->kvCompletedBlocks;
        if (parent.completed.insert(block).second)
            KvRoutingStats::Get().RecordRx(h.transfer, block, now);
    }
    if (parent.completed.size() == parent.blocks) {
        parent.expiry = now + cfg.tombstoneNs;
        if (!parent.done) {
            parent.done = true;
            KvRoutingStats::Get().RecordRx(h.transfer, std::numeric_limits<uint32_t>::max(), now);
        }
    }
    const uint64_t target = h.flags & KvBlockHeader::MEGA ? count * cfg.blockBytes : cfg.blockBytes;
    rx->kvDone = rx->ReceiverNextExpectedSeq == target;
    m_kvRxExpiry[((uint64_t)rx->dip << 32) | ((uint64_t)rx->m_ecn_source.qIndex << 16) |
                 rx->dport] = now + (rx->ReceiverNextExpectedSeq == target
                                    ? cfg.tombstoneNs : cfg.timeoutNs);
    if ((!m_kvRxExpiry.empty() || parent.expiry) && m_kvRxCleanup.IsExpired())
        m_kvRxCleanup = Simulator::Schedule(NanoSeconds(cfg.tombstoneNs),
                                             &RdmaHw::CleanupKvReceivers, this);
}
void RdmaHw::CleanupKvReceivers() {
    const uint64_t now = Simulator::Now().GetNanoSeconds();
    for (auto it = m_kvRxExpiry.begin(); it != m_kvRxExpiry.end();) {
        if (it->second <= now) {
            m_rxQpMap.erase(it->first); it = m_kvRxExpiry.erase(it);
        } else ++it;
    }
    for (auto it = m_kvRxParents.begin(); it != m_kvRxParents.end();) {
        if (it->second.expiry && it->second.expiry <= now) it = m_kvRxParents.erase(it);
        else ++it;
    }
    if (!m_kvRxExpiry.empty())
        m_kvRxCleanup = Simulator::Schedule(NanoSeconds(KvRoutingConfig::Get().tombstoneNs),
                                             &RdmaHw::CleanupKvReceivers, this);
}
void RdmaHw::CheckKvProgress(Ptr<RdmaKvTransfer> p) {
    if (p->done) return;
    const auto& cfg = KvRoutingConfig::Get();
    if (!p->active) {
        p->watchdog = Simulator::Schedule(NanoSeconds(cfg.warnNs),
                                           &RdmaHw::CheckKvProgress, this, p);
        return; // 等待源端额度不是网络无进展。
    }
    const uint64_t elapsed = Simulator::Now().GetNanoSeconds() - p->lastProgress;
    if (elapsed >= cfg.warnNs && !p->warned) {
        p->warned = true;
        std::cerr << "KV parent stalled: request=" << p->request << " transfer=" << p->id << '\n';
    }
    if (elapsed >= cfg.timeoutNs) {
        auto& s = KvRoutingStats::Get();
        s.blocks.flush(); s.parents.flush(); s.decisions.flush(); s.feedback.flush(); s.ports.flush();
        NS_ABORT_MSG("KV parent timeout; request=" << p->request);
    }
    p->watchdog = Simulator::Schedule(NanoSeconds(cfg.warnNs),
                                       &RdmaHw::CheckKvProgress, this, p);
}
}
