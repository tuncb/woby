// Primary keys are implicit in their source-position slots. Only seams keep
// full keys; buckets contain seam slots rather than global render IDs.
constexpr uint32_t invalidUvBit = uint32_t{1} << 31;
constexpr uint32_t renderIdMask = invalidUvBit - 1;
constexpr uint32_t emptyBucket = std::numeric_limits<uint32_t>::max();
static_assert(std::numeric_limits<SceneBufferSize>::max() / sizeof(Vertex) < invalidUvBit);

struct PrimaryEntry {
    uint32_t id = emptyBucket;
    int normal = -1;
    int texcoord = -1;
};
struct SecondaryEntry {
    IndexKey key;
    uint32_t taggedId = emptyBucket;
};
struct VertexIndexTable {
    std::vector<SecondaryEntry> keys;
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
    // Match production: validate the float conversion, not the input double.
    const auto u = table.preciseUvs ? static_cast<float>(table.preciseUvs[uv][0]) : table.floatUvs[uv * 2];
    const auto v = table.preciseUvs ? static_cast<float>(table.preciseUvs[uv][1]) : table.floatUvs[uv * 2 + 1];
    return std::isfinite(u) && std::isfinite(v) ? id : id | invalidUvBit;
}

void growSecondary(VertexIndexTable& table)
{
    std::vector<uint32_t> buckets(std::max(size_t{16}, table.buckets.size() * 2), emptyBucket);
    for (const auto slot : table.buckets) {
        if (slot == emptyBucket) { continue; }
        size_t bucket = indexHash(table.keys[slot].key) & (buckets.size() - 1);
        while (buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & (buckets.size() - 1); }
        buckets[bucket] = slot;
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
    auto& entry = table.entries[static_cast<size_t>(key.vertex)];
    if (entry.id == emptyBucket) {
        (void)sceneBufferBytes(size_t(table.next) + 1, sizeof(Vertex));
        const auto tagged = tagVertex(table, key, table.next);
        entry.normal = key.normal;
        entry.texcoord = key.texcoord;
        entry.id = tagged;
        ++table.next;
        return tagged;
    }
    if (entry.normal == key.normal && entry.texcoord == key.texcoord) { return entry.id; }
    if (table.buckets.empty()) { growSecondary(table); }
    size_t bucket = indexHash(key) & (table.buckets.size() - 1);
    while (table.buckets[bucket] != emptyBucket) {
        const auto& seam = table.keys[table.buckets[bucket]];
        if (seam.key == key) { return seam.taggedId; }
        bucket = (bucket + 1) & (table.buckets.size() - 1);
    }
    (void)sceneBufferBytes(size_t(table.next) + 1, sizeof(Vertex));
    if (table.secondaryCount >= table.buckets.size() * 3 / 4) {
        growSecondary(table);
        bucket = indexHash(key) & (table.buckets.size() - 1);
        while (table.buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & (table.buckets.size() - 1); }
    }
    const auto tagged = tagVertex(table, key, table.next);
    const auto slot = static_cast<uint32_t>(table.keys.size());
    table.keys.push_back({key, tagged});
    table.buckets[bucket] = slot;
    ++table.next;
    ++table.secondaryCount;
    return tagged;
}

void reserveIndexTable(VertexIndexTable& table, size_t positions, size_t corners, bool positionOnly)
{
    table.direct = positionOnly;
    if (positionOnly) { table.primary.assign(positions, emptyBucket); }
    else {
        table.entries.resize(positions);
        // Reserving one seam key per position would remove the memory saving.
        table.keys.reserve(std::min({positions, corners, size_t{1024}}));
    }
}
