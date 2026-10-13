// This file is inserted into the generated loader's anonymous namespace.
// Bit 31 is internal UV availability. SceneBufferSize limits render IDs to
// fewer than 2^27 values for the 32-byte Vertex layout.
constexpr uint32_t invalidUvBit = uint32_t{1} << 31;
constexpr uint32_t renderIdMask = invalidUvBit - 1;
constexpr uint32_t emptyBucket = std::numeric_limits<uint32_t>::max();
static_assert(std::numeric_limits<SceneBufferSize>::max() / sizeof(Vertex) < invalidUvBit);

struct PrimaryEntry {
    uint32_t id = emptyBucket;
    int normal = -1;
    int texcoord = -1;
};
struct VertexIndexTable {
    std::vector<IndexKey> keys;
    std::vector<uint32_t> primary, buckets;
    std::vector<PrimaryEntry> entries;
    const std::array<double, 2>* preciseUvs = nullptr;
    const float* floatUvs = nullptr;
    size_t secondaryCount = 0;
    bool direct = false;
    uint32_t next = 0;
};

size_t indexHash(const IndexKey& key)
{
    uint64_t hash = uint64_t(static_cast<uint32_t>(key.vertex)) * 0x9e3779b185ebca87ull;
    hash ^= uint64_t(static_cast<uint32_t>(key.normal)) * 0xc2b2ae3d27d4eb4full;
    hash ^= uint64_t(static_cast<uint32_t>(key.texcoord)) * 0x165667b19e3779f9ull;
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    return static_cast<size_t>(hash ^ (hash >> 33));
}
uint32_t tagVertex(const VertexIndexTable& table, const IndexKey& key, uint32_t id)
{
    if (!corner_mapping::validateUniqueUvs) { return id; }
    if (key.texcoord < 0) { return id | invalidUvBit; }
    const auto uv = static_cast<size_t>(key.texcoord);
    const auto u = table.preciseUvs ? static_cast<float>(table.preciseUvs[uv][0]) : table.floatUvs[uv * 2];
    const auto v = table.preciseUvs ? static_cast<float>(table.preciseUvs[uv][1]) : table.floatUvs[uv * 2 + 1];
    return std::isfinite(u) && std::isfinite(v) ? id : id | invalidUvBit;
}
void growSecondary(VertexIndexTable& table)
{
    std::vector<uint32_t> buckets(std::max(size_t{16}, table.buckets.size() * 2), emptyBucket);
    for (const auto tagged : table.buckets) {
        if (tagged == emptyBucket) { continue; }
        const auto id = tagged & renderIdMask;
        size_t bucket = indexHash(table.keys[id]) & (buckets.size() - 1);
        while (buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & (buckets.size() - 1); }
        buckets[bucket] = tagged;
    }
    table.buckets = std::move(buckets);
}
uint32_t vertexIndex(VertexIndexTable& table, const IndexKey& key)
{
    if (table.direct) {
        auto& primary = table.primary[static_cast<size_t>(key.vertex)];
        if (primary == emptyBucket) {
            (void)sceneBufferBytes(size_t(table.next) + 1, sizeof(Vertex));
            primary = table.next++;
        }
        return primary;
    }
#ifdef CORNER_MAPPING_DENSE
    auto& entry = table.entries[static_cast<size_t>(key.vertex)];
    auto& primary = entry.id;
#else
    auto& primary = table.primary[static_cast<size_t>(key.vertex)];
#endif
    if (primary == emptyBucket) {
        (void)sceneBufferBytes(table.keys.size() + 1, sizeof(Vertex));
        primary = tagVertex(table, key, static_cast<uint32_t>(table.keys.size()));
        table.keys.push_back(key);
#ifdef CORNER_MAPPING_DENSE
        entry.normal = key.normal;
        entry.texcoord = key.texcoord;
#endif
        return primary;
    }
#ifdef CORNER_MAPPING_DENSE
    if (entry.normal == key.normal && entry.texcoord == key.texcoord) { return primary; }
#else
    if (table.keys[primary & renderIdMask] == key) { return primary; }
#endif
    if (table.buckets.empty()) { growSecondary(table); }
    size_t bucket = indexHash(key) & (table.buckets.size() - 1);
    while (table.buckets[bucket] != emptyBucket) {
        const auto tagged = table.buckets[bucket];
        if (table.keys[tagged & renderIdMask] == key) { return tagged; }
        bucket = (bucket + 1) & (table.buckets.size() - 1);
    }
    (void)sceneBufferBytes(table.keys.size() + 1, sizeof(Vertex));
    if (table.secondaryCount >= table.buckets.size() * 3 / 4) {
        growSecondary(table);
        bucket = indexHash(key) & (table.buckets.size() - 1);
        while (table.buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & (table.buckets.size() - 1); }
    }
    const auto tagged = tagVertex(table, key, static_cast<uint32_t>(table.keys.size()));
    table.keys.push_back(key);
    table.buckets[bucket] = tagged;
    ++table.secondaryCount;
    return tagged;
}
void reserveIndexTable(VertexIndexTable& table, size_t positions, size_t corners, bool positionOnly)
{
    table.direct = positionOnly;
#ifdef CORNER_MAPPING_DENSE
    if (!positionOnly) { table.entries.resize(positions); }
    else { table.primary.assign(positions, emptyBucket); }
#else
    table.primary.assign(positions, emptyBucket);
#endif
    if (!positionOnly) { table.keys.reserve(std::min(positions, corners)); }
}
