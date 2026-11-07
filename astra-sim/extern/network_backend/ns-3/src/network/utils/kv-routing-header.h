#ifndef KV_ROUTING_HEADER_H
#define KV_ROUTING_HEADER_H

#include "ns3/header.h"
#include <limits>
#include <vector>

namespace ns3 {

class KvBlockHeader : public Header {
public:
    static constexpr uint16_t UDP_PORT = 49500;
    static constexpr uint32_t SIZE = 72;
    enum : uint8_t { MEGA = 1, BLOCK = 2, ACK = 4 };
    uint8_t flags = BLOCK;
    uint64_t transfer = 0, request = 0, parentBytes = 0, rangeOffset = 0;
    uint32_t epoch = 0, block = 0, blocks = 0, parentBlocks = 0;
    uint32_t offset = 0, validBytes = 0;
    uint32_t cookie = std::numeric_limits<uint32_t>::max(), tag = 0;
    bool valid = true;
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator) const override;
    uint32_t Deserialize(Buffer::Iterator) override;
    void Print(std::ostream&) const override;
};

struct KvLoadRecord {
    uint32_t destination = 0;
    uint64_t horizon = 0, floor = 0, oldest = 0;
    uint32_t flags = 0;
    enum : uint32_t { FRESH = 1 };
};

class KvLoadHeader : public Header {
public:
    static constexpr uint8_t PROTOCOL = 0xfa;
    static constexpr uint32_t FIXED_SIZE = 44, RECORD_SIZE = 32;
    uint32_t sender = 0;
    uint64_t boot = 0, sequence = 0, generated = 0;
    uint16_t fragment = 0, fragments = 1;
    std::vector<KvLoadRecord> records;
    bool valid = true;
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator) const override;
    uint32_t Deserialize(Buffer::Iterator) override;
    void Print(std::ostream&) const override;
private:
    static uint32_t Crc(const std::vector<uint8_t>&);
    void Write(Buffer::Iterator, uint32_t crc) const;
};

}
#endif
