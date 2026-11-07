#include "kv-routing-header.h"
#include "ns3/buffer.h"

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(KvBlockHeader);
NS_OBJECT_ENSURE_REGISTERED(KvLoadHeader);

TypeId KvBlockHeader::GetTypeId() {
    static TypeId t = TypeId("ns3::KvBlockHeader").SetParent<Header>()
        .AddConstructor<KvBlockHeader>();
    return t;
}
TypeId KvBlockHeader::GetInstanceTypeId() const { return GetTypeId(); }
uint32_t KvBlockHeader::GetSerializedSize() const { return SIZE; }
void KvBlockHeader::Print(std::ostream& out) const { out << transfer << ':' << block; }
void KvBlockHeader::Serialize(Buffer::Iterator i) const {
    i.WriteU8(1); i.WriteU8(flags); i.WriteHtonU16(SIZE);
    for (auto x : {transfer, request, parentBytes, rangeOffset}) i.WriteHtonU64(x);
    for (auto x : {epoch, block, blocks, parentBlocks, offset, validBytes, cookie, tag})
        i.WriteHtonU32(x);
    i.WriteHtonU32(0);
}
uint32_t KvBlockHeader::Deserialize(Buffer::Iterator i) {
    if (i.GetRemainingSize() < SIZE) { valid = false; return 0; }
    const auto version = i.ReadU8();
    flags = i.ReadU8();
    const auto size = i.ReadNtohU16();
    valid = version == 1 && size == SIZE;
    transfer = i.ReadNtohU64(); request = i.ReadNtohU64();
    parentBytes = i.ReadNtohU64(); rangeOffset = i.ReadNtohU64();
    epoch = i.ReadNtohU32(); block = i.ReadNtohU32();
    blocks = i.ReadNtohU32(); parentBlocks = i.ReadNtohU32();
    offset = i.ReadNtohU32(); validBytes = i.ReadNtohU32();
    cookie = i.ReadNtohU32(); tag = i.ReadNtohU32(); i.ReadNtohU32();
    valid = valid && ((flags & (MEGA | BLOCK)) == MEGA ||
                      (flags & (MEGA | BLOCK)) == BLOCK);
    return valid ? SIZE : 0;
}

TypeId KvLoadHeader::GetTypeId() {
    static TypeId t = TypeId("ns3::KvLoadHeader").SetParent<Header>()
        .AddConstructor<KvLoadHeader>();
    return t;
}
TypeId KvLoadHeader::GetInstanceTypeId() const { return GetTypeId(); }
uint32_t KvLoadHeader::GetSerializedSize() const {
    return FIXED_SIZE + RECORD_SIZE * records.size();
}
void KvLoadHeader::Print(std::ostream& out) const { out << sender << ':' << sequence; }
uint32_t KvLoadHeader::Crc(const std::vector<uint8_t>& bytes) {
    uint32_t crc = 0xffffffff;
    for (auto b : bytes) {
        crc ^= b;
        for (unsigned k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
void KvLoadHeader::Write(Buffer::Iterator i, uint32_t crc) const {
    i.WriteU8(1); i.WriteU8(0); i.WriteHtonU16(FIXED_SIZE);
    i.WriteHtonU32(sender); i.WriteHtonU64(boot);
    i.WriteHtonU64(sequence); i.WriteHtonU64(generated);
    i.WriteHtonU16(fragment); i.WriteHtonU16(fragments);
    i.WriteHtonU16(records.size()); i.WriteHtonU16(0); i.WriteHtonU32(crc);
    for (const auto& r : records) {
        i.WriteHtonU32(r.destination);
        i.WriteHtonU64(r.horizon); i.WriteHtonU64(r.floor);
        i.WriteHtonU64(r.oldest); i.WriteHtonU32(r.flags);
    }
}
void KvLoadHeader::Serialize(Buffer::Iterator i) const {
    Buffer buffer;
    buffer.AddAtStart(GetSerializedSize());
    Write(buffer.Begin(), 0);
    std::vector<uint8_t> bytes(GetSerializedSize());
    buffer.Begin().Read(bytes.data(), bytes.size());
    Write(i, Crc(bytes));
}
uint32_t KvLoadHeader::Deserialize(Buffer::Iterator i) {
    records.clear();
    if (i.GetRemainingSize() < FIXED_SIZE) { valid = false; return 0; }
    auto start = i;
    const auto remaining = i.GetRemainingSize();
    const auto version = i.ReadU8();
    i.ReadU8();
    const auto headerSize = i.ReadNtohU16();
    valid = version == 1 && headerSize == FIXED_SIZE;
    sender = i.ReadNtohU32(); boot = i.ReadNtohU64();
    sequence = i.ReadNtohU64(); generated = i.ReadNtohU64();
    fragment = i.ReadNtohU16(); fragments = i.ReadNtohU16();
    const auto count = i.ReadNtohU16(); i.ReadNtohU16();
    const auto crc = i.ReadNtohU32();
    const uint32_t size = FIXED_SIZE + count * RECORD_SIZE;
    if (!valid || size > remaining || !fragments || fragment >= fragments) {
        valid = false; return 0;
    }
    for (unsigned n = 0; n < count; ++n) {
        KvLoadRecord r;
        r.destination = i.ReadNtohU32();
        r.horizon = i.ReadNtohU64(); r.floor = i.ReadNtohU64();
        r.oldest = i.ReadNtohU64(); r.flags = i.ReadNtohU32();
        valid = valid && r.floor <= r.horizon;
        records.push_back(r);
    }
    std::vector<uint8_t> bytes(size);
    start.Read(bytes.data(), size);
    for (unsigned n = 40; n < 44; ++n) bytes[n] = 0;
    valid = valid && Crc(bytes) == crc;
    return valid ? size : 0;
}
}
