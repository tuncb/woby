"""Research-only mapping counters and deterministic split/gather experiments."""


def replace(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new)


def instrument(text):
    text = '#include "mapping_diagnostics.h"\n' + text
    text = replace(text, '    uint64_t hash =', '    ++mapping_probe::counters.hash_calls;\n    uint64_t hash =')
    text = replace(text, '    std::vector<uint32_t> buckets(std::max', '''    const auto growthBegin = mapping_probe::Clock::now();
    ++mapping_probe::counters.rehashes;
    mapping_probe::counters.rehashed_entries += table.secondaryCount;
    std::vector<uint32_t> buckets(std::max''')
    text = replace(text, '        while (buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & (buckets.size() - 1); }', '''        ++mapping_probe::counters.rehash_slots;
        while (buckets[bucket] != emptyBucket) {
            ++mapping_probe::counters.rehash_slots;
            bucket = (bucket + 1) & (buckets.size() - 1);
        }''')
    text = replace(text, '    table.buckets = std::move(buckets);', '''    table.buckets = std::move(buckets);
    mapping_probe::counters.rehash_ms += mapping_probe::elapsed(growthBegin);''')
    text = replace(text, '        if (primary == emptyBucket) { primary = table.next++; }', '''        if (primary == emptyBucket) { ++mapping_probe::counters.direct_new; primary = table.next++; }
        else { ++mapping_probe::counters.direct_hit; }''')
    text = replace(text, '    if (primary == emptyBucket) {\n        primary =', '    if (primary == emptyBucket) {\n        ++mapping_probe::counters.primary_new;\n        primary =')
    text = replace(text, '    if (table.keys[primary] == key) { return primary; }', '''    if (table.keys[primary] == key) { ++mapping_probe::counters.primary_hit; return primary; }
    ++mapping_probe::counters.secondary_lookups;''')
    text = replace(text, '    while (table.buckets[bucket] != emptyBucket) {\n        const auto id', '    uint64_t probes = 1;\n    while (table.buckets[bucket] != emptyBucket) {\n        const auto id')
    text = replace(text, '        if (table.keys[id] == key) { return id; }', '''        if (table.keys[id] == key) {
            ++mapping_probe::counters.secondary_hit;
            mapping_probe::recordProbes(probes);
            return id;
        }''')
    text = replace(text, '        bucket = (bucket + 1) & (table.buckets.size() - 1);\n    }', '        ++probes;\n        bucket = (bucket + 1) & (table.buckets.size() - 1);\n    }')
    text = replace(text, '    if (table.secondaryCount >=', '''    ++mapping_probe::counters.secondary_new;
    mapping_probe::recordProbes(probes);
    if (table.secondaryCount >=''')
    text = replace(text, '        while (table.buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & (table.buckets.size() - 1); }', '''        ++mapping_probe::counters.growth_placement_slots;
        while (table.buckets[bucket] != emptyBucket) {
            ++mapping_probe::counters.growth_placement_slots;
            bucket = (bucket + 1) & (table.buckets.size() - 1);
        }''')
    assert text.count('table.keys.push_back(key);') == 2
    text = text.replace('table.keys.push_back(key);', 'mapping_probe::trackedPush(table.keys, key, mapping_probe::counters.key_growth);')
    text = replace(text, '            mesh.vertices.push_back(vertex);', '            mapping_probe::trackedPush(mesh.vertices, vertex, mapping_probe::counters.vertex_growth);')
    return text


def split_gather(text, workers):
    text = '#include <thread>\n' + text
    # The ordinary direct path has no keys. Record them only for this experiment
    # so the gather pass can preserve first-use IDs without scanning corners again.
    text = replace(text, '        if (primary == emptyBucket) { primary = table.next++; }',
                   '        if (primary == emptyBucket) { primary = table.next++; table.keys.push_back(key); }')
    text = replace(text, '    if (!table.direct) { table.keys.reserve(std::min(positions, corners)); }',
                   '    table.keys.reserve(std::min(positions, corners));')
    text = replace(text, '    mesh.vertices.reserve(vertexCapacity);', '    (void)vertexCapacity; // Exact vertex count becomes available after lookup.')
    start = text.index('            if (mappedIndex < mesh.vertices.size())')
    # Find the outer corner-loop end, after the gathered vertex has been appended.
    end = text.index('\n        }', text.index('            mesh.indices.push_back(newIndex);', start))
    gather_start = text.index('            Vertex vertex{};', start)
    gather_end = text.index('            const uint32_t newIndex', gather_start)
    gather = text[gather_start:gather_end]
    gather = gather.replace('index.position_index', 'key.vertex').replace('index.normal_index', 'key.normal').replace('index.texcoord_index', 'key.texcoord')
    text = text[:start] + '            mesh.indices.push_back(mappedIndex);' + text[end:]
    insertion = '''
    mapping_probe::mark("split_lookup_ms");
    mesh.vertices.resize(vertexMap.keys.size());
    mapping_probe::mark("split_vertex_allocate_ms");
    const auto gatherRange = [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const auto& key = vertexMap.keys[i];
''' + gather + '''            mesh.vertices[i] = vertex;
        }
    };
'''
    if workers == 1:
        insertion += '    gatherRange(0, mesh.vertices.size());\n'
    else:
        insertion += f'''    {{
        const size_t workerCount = std::min(size_t{{{workers}}}, mesh.vertices.size());
        std::vector<std::jthread> threads;
        threads.reserve(workerCount);
        for (size_t worker = 0; worker < workerCount; ++worker) {{
            threads.emplace_back(gatherRange, mesh.vertices.size() * worker / workerCount,
                mesh.vertices.size() * (worker + 1) / workerCount);
        }}
    }} // Joining and thread startup are included in gather wall time.
'''
    insertion += '''    mapping_probe::mark("split_gather_ms");
    // Restore the enclosing timer: map_loop_ms includes all three split passes.
    mapping_probe::previous = mappingLoopBegin;
'''
    text = replace(text, '    if (empty(mesh)) {', insertion + '\n    if (empty(mesh)) {')
    text = replace(text, '    for (size_t shapeIndex', '    const auto mappingLoopBegin = mapping_probe::Clock::now();\n    for (size_t shapeIndex')
    return text
