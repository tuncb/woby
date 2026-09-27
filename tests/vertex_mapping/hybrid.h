// Research-only alternatives; included inside the generated loader's namespace.
struct VertexIndexTable {
    std::vector<IndexKey> keys;
    std::vector<uint32_t> primary, buckets;
    size_t secondaryCount = 0;
    bool direct = false;
    uint32_t next = 0;
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

void growSecondary(VertexIndexTable& table)
{
    std::vector<uint32_t> buckets(std::max(size_t{16}, table.buckets.size() * 2), emptyBucket);
    for (const auto id : table.buckets) {
        if (id == emptyBucket) { continue; }
        size_t bucket = indexHash(table.keys[id]) & (buckets.size() - 1);
        while (buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & (buckets.size() - 1); }
        buckets[bucket] = id;
    }
    table.buckets = std::move(buckets);
}

uint32_t vertexIndex(VertexIndexTable& table, const IndexKey& key)
{
    auto& primary = table.primary[static_cast<size_t>(key.vertex)];
    if (table.direct) {
        if (primary == emptyBucket) { primary = table.next++; }
        return primary;
    }
    if (primary == emptyBucket) {
        primary = static_cast<uint32_t>(table.keys.size());
        table.keys.push_back(key);
        return primary;
    }
    if (table.keys[primary] == key) { return primary; }
    if (table.buckets.empty()) { growSecondary(table); }
    size_t bucket = indexHash(key) & (table.buckets.size() - 1);
    while (table.buckets[bucket] != emptyBucket) {
        const auto id = table.buckets[bucket];
        if (table.keys[id] == key) { return id; }
        bucket = (bucket + 1) & (table.buckets.size() - 1);
    }
    if (table.secondaryCount >= table.buckets.size() * 3 / 4) {
        growSecondary(table);
        bucket = indexHash(key) & (table.buckets.size() - 1);
        while (table.buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & (table.buckets.size() - 1); }
    }
    const auto id = static_cast<uint32_t>(table.keys.size());
    table.keys.push_back(key);
    table.buckets[bucket] = id;
    ++table.secondaryCount;
    return id;
}
