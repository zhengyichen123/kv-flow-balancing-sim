#ifndef SWITCH_NODE_H
#define SWITCH_NODE_H

#include <unordered_map>
#include <ns3/node.h>
#include "qbb-net-device.h"
#include "switch-mmu.h"
#include "pint.h"
#include "kv-routing-policy.h"
#include "ldlb-routing.h"
#include "ns3/random-variable-stream.h"
#include <map>
#include <set>

namespace ns3 {

class Packet;

class SwitchNode : public Node{
	static const uint32_t pCnt = 1025;	// Number of ports used
	static const uint32_t qCnt = 8;	// Number of queues/priorities used
	uint32_t m_ecmpSeed;
	std::unordered_map<uint32_t, std::vector<int> > m_rtTable; // map from ip address (u32) to possible ECMP port (index of dev)

	// monitor of PFC
	uint32_t m_bytes[pCnt][pCnt][qCnt]; // m_bytes[inDev][outDev][qidx] is the bytes from inDev enqueued for outDev at qidx
	
	uint64_t m_txBytes[pCnt]; // counter of tx bytes

	uint32_t m_lastPktSize[pCnt];
	uint64_t m_lastPktTs[pCnt]; // ns
	double m_u[pCnt];

protected:
	bool m_ecnEnabled;
	uint32_t m_ccMode;
	uint64_t m_maxRtt;

	uint32_t m_ackHighPrio; // set high priority for ACK/NACK

private:
	int GetOutDev(Ptr<const Packet>, CustomHeader &ch);
	void SendToDev(Ptr<Packet>p, CustomHeader &ch);
	static uint32_t EcmpHash(const uint8_t* key, size_t len, uint32_t seed);
	void CheckAndSendPfc(uint32_t inDev, uint32_t qIndex);
	void CheckAndSendResume(uint32_t inDev, uint32_t qIndex);
public:
	Ptr<SwitchMmu> m_mmu;

	static TypeId GetTypeId (void);
	SwitchNode();
	void SetEcmpSeed(uint32_t seed);
	void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
	void ClearTable();
	bool SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch);
	void SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p);
        void InitializeKvRouting();
        void ReceiveKvFeedback(uint32_t inPort, Ptr<Packet> packet);
        void ReceiveLdlbLabel(uint32_t inPort, Ptr<Packet> packet);

	// for approximate calc in PINT
	int logres_shift(int b, int l);
	int log2apprx(int x, int b, int m, int l); // given x of at most b bits, use most significant m bits of x, calc the result in l bits
private:
        struct KvPin {
            int port = -1;
            uint64_t remaining = 0, expiry = 0, packetCount = 0;
            std::vector<uint64_t> admitted;
        };
        struct KvCached {
            KvLoadRecord record;
            uint64_t generated = 0;
        };
        struct KvAssembly {
            uint64_t sequence = 0, generated = 0;
            uint16_t fragments = 0;
            std::map<uint16_t, std::vector<KvLoadRecord>> parts;
        };
        struct KvPending {
            uint64_t sequence = 0, generated = 0;
            std::vector<KvLoadRecord> records;
            uint32_t next = 0;
        };
        struct KvBucket {
            uint64_t tokens = 8000, updated = 0, lastTx = 0;
        };
        std::unique_ptr<KvRoutingPolicy> m_kvPolicy;
        LdlbState m_ldlb;
        Ptr<UniformRandomVariable> m_ldlbRandom;
        void PollLdlbBroadcast();
        void SampleLdlb(uint32_t port, uint32_t priority);
        std::map<std::pair<uint64_t, uint32_t>, KvPin> m_kvPins;
        std::map<std::pair<uint32_t, uint32_t>, KvCached> m_kvCache;
        std::map<uint32_t, KvAssembly> m_kvAssemblies;
        std::map<uint32_t, KvPending> m_kvPending;
        std::map<uint32_t, KvBucket> m_kvBuckets;
        std::map<uint32_t, uint64_t> m_kvReceivedSequence;
        std::map<uint32_t, std::vector<KvLoadRecord>> m_kvLastAdvertised;
        std::map<std::pair<uint32_t, int>, uint64_t> m_kvPropagation;
        std::set<uint32_t> m_kvPropagationPending;
        std::vector<uint64_t> m_kvReserved = std::vector<uint64_t>(pCnt, 0);
        std::vector<uint64_t> m_kvSampleBytes = std::vector<uint64_t>(pCnt, 0);
        EventId m_kvPeriodic, m_kvEvent, m_kvDrain, m_kvMaintenance;
        uint64_t m_kvSequence = 0;
        bool m_kvLastDecision = false;
        KvRoutingContext m_kvContext;
        KvDecision m_kvDecision;
        Ptr<Node> KvNeighbor(uint32_t port) const;
        uint64_t InitializeKvPropagation(uint32_t destination);
        KvRoutingContext MakeKvContext(uint32_t destination, const KvBlockHeader* block = nullptr);
        int SelectKvPort(const CustomHeader& ch);
        void CommitKvPacket(int port, uint32_t inPort, Ptr<Packet>& packet, CustomHeader& ch);
        void MarkKvDirty();
        void EmitKvFeedback(bool periodic);
        void DrainKvFeedback();
        void MaintainKvState();
};

} /* namespace ns3 */

#endif /* SWITCH_NODE_H */
