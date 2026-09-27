// Historical global tuple table, retained only as a benchmark control.
struct VertexIndexTable {
    std::vector<IndexKey> keys;
    std::vector<uint32_t> buckets;
};
constexpr uint32_t emptyBucket = std::numeric_limits<uint32_t>::max();

size_t indexHash(const IndexKey& key)
{
    uint64_t hash = uint64_t(static_cast<uint32_t>(key.vertex)) * 0x9e3779b185ebca87ull;
    hash ^= uint64_t(static_cast<uint32_t>(key.normal)) * 0xc2b2ae3d27d4eb4full;
    hash ^= uint64_t(static_cast<uint32_t>(key.texcoord)) * 0x165667b19e3779f9ull;
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    return static_cast<size_t>(hash ^ (hash >> 33));
}

void reserveIndexTable(VertexIndexTable& table, size_t capacity)
{
    table.keys.reserve(capacity);
    table.buckets.assign(std::bit_ceil(std::max(size_t{16}, capacity + capacity / 2)), emptyBucket);
    const size_t mask = table.buckets.size() - 1;
    for (size_t i = 0; i < table.keys.size(); ++i) {
        size_t bucket = indexHash(table.keys[i]) & mask;
        while (table.buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & mask; }
        table.buckets[bucket] = static_cast<uint32_t>(i);
    }
}

uint32_t vertexIndex(VertexIndexTable& table, const IndexKey& key)
{
    size_t mask = table.buckets.size() - 1;
    size_t bucket = indexHash(key) & mask;
    while (table.buckets[bucket] != emptyBucket) {
        const auto index = table.buckets[bucket];
        if (table.keys[index] == key) { return index; }
        bucket = (bucket + 1) & mask;
    }
    if (table.keys.size() >= table.buckets.size() * 3 / 4) {
        reserveIndexTable(table, table.buckets.size());
        mask = table.buckets.size() - 1;
        bucket = indexHash(key) & mask;
        while (table.buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & mask; }
    }
    const auto index = static_cast<uint32_t>(table.keys.size());
    table.keys.push_back(key);
    table.buckets[bucket] = index;
    return index;
}

