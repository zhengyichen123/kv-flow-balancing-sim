#ifndef KV_ROUTING_POLICY_H
#define KV_ROUTING_POLICY_H

#include "ns3/kv-routing-header.h"
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ns3 {

enum class KvTransport { LEGACY, REQUEST, BLOCK };

struct KvRoutingConfig {
    std::string mode = "ECMP";
    uint32_t blockBytes = 2097152, mtu = 4000, parentWindow = 4, hostWindow = 32;
    uint64_t refreshNs = 16000, minGapNs = 4000, ttlNs = 64000, tieNs = 5000;
    uint64_t tombstoneNs = 2000000, warnNs = 10000000, timeoutNs = 100000000;
    uint64_t seed = 42, budgetPpm = 5000;
    uint32_t ldlbGuard = 1, ldlbEnter = 2, ldlbExit = 3, ldlbDampening = 1, ldlbRounds = 2;
    double ldlbAlpha = 0.25;
    uint64_t ldlbGapNs = 50000;
    std::string output;
    static KvRoutingConfig& Get();
    bool Read(const std::string&, std::istream&);
    void Validate() const;
};

struct KvPortView {
    int port = -1;
    uint64_t queue = 0, serial = 0, reserved = 0, bandwidth = 0;
    uint64_t propagation = 0;
    uint64_t horizon = 0, floor = 0, age = 0, oldest = 0;
    bool fresh = false, terminal = false;
    bool localCongested = false, remoteCongested = false, up = true;
};
struct KvRoutingContext {
    uint32_t switchId = 0, destination = 0, block = 0, epoch = 0;
    uint64_t transfer = 0, request = 0, now = 0, wireBytes = 0;
    int ecmpPort = -1;
    uint32_t ldlbTieHash = 0;
    std::vector<KvPortView> ports;
};
struct KvDecision { int port = -1; bool fallback = false; uint64_t cost = 0; };

// 策略只接收本地状态与已收到的反馈；接口不提供远端交换机指针。
class KvRoutingPolicy {
public:
    using Factory = std::function<std::unique_ptr<KvRoutingPolicy>()>;
    virtual ~KvRoutingPolicy() = default;
    virtual KvTransport Transport() const = 0;
    virtual bool UsesFeedback() const { return false; }
    virtual bool UsesLabels() const { return false; }
    virtual KvDecision Select(const KvRoutingContext&) const = 0;
    virtual KvLoadRecord Advertise(const KvRoutingContext&) const;
    static void Register(const std::string&, Factory);
    static std::unique_ptr<KvRoutingPolicy> Create(const std::string&);
    static std::vector<std::string> Names();
    static uint64_t ServiceNs(uint64_t bytes, uint64_t bitsPerSecond);
    static uint64_t Hash(const KvRoutingContext&);
};

// 此处的全局关联仅供输出统计使用，选路与发送控制不得读取它。
class KvRoutingStats {
public:
    static KvRoutingStats& Get();
    void Open(const KvRoutingConfig&);
    void RecordRx(uint64_t transfer, uint32_t block, uint64_t now);
    uint64_t RxTime(uint64_t transfer, uint32_t block) const;
    void Forget(uint64_t transfer);
    std::ofstream blocks, parents, decisions, feedback, ports;
private:
    std::map<std::pair<uint64_t, uint32_t>, uint64_t> m_rx;
};

}
#endif
