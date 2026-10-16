#pragma once
// Included by obj_corner_mapping.h after the serial resolver and shared types.
#include <bit>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace woby::obj_mapping {
// Temporary IDs are first-corner offsets, with the same UV flag as final IDs.
template<bool Precise>
inline uint32_t discoverSeam(Table& table, const Key& key, uint32_t first,
    const std::array<double,2>* preciseUvs, const float* floatUvs, bool finite) {
    if (table.buckets.empty()) { growBuckets(table); }
    auto bucket = hashKey(key) & (table.buckets.size()-1);
    while (table.buckets[bucket] != empty) {
        const auto& seam = table.seams[table.buckets[bucket]];
        if (seam.key == key) { return seam.id; }
        bucket = (bucket+1) & (table.buckets.size()-1);
    }
    if (table.seams.size() >= table.buckets.size()*3/4) {
        growBuckets(table); bucket = hashKey(key) & (table.buckets.size()-1);
        while (table.buckets[bucket] != empty) { bucket = (bucket+1) & (table.buckets.size()-1); }
    }
    const auto id = tagId<Precise>(key,first,preciseUvs,floatUvs,finite);
    table.buckets[bucket] = static_cast<uint32_t>(table.seams.size());
    table.seams.push_back({key,id}); ++table.next;
    return id;
}

template<bool Precise>
inline FaceResolutionTimes resolveParallelFaces(const rapidobj::Result& result, Table& table,
    MeshGeometry& geometry, SourceMeshData& source, const rapidobj::GeometryBuffers* buffers,
    const ModelLoadProgressCallback& progress, size_t corners, bool deferred, size_t workers, size_t discoverySize, size_t totalIndices) {
    FaceResolutionTimes times;
    if (!workers || workers > 8 || !discoverySize) { throw std::invalid_argument("Invalid OBJ discovery partition size."); }
    if (!totalIndices) { totalIndices = corners; }
    auto start = Clock::now();
    (void)sceneBufferBytes(corners,sizeof(uint32_t));
    const auto positions = sourcePositions(source).size();
    table.primary.resize(positions);
    geometry.indices.resize(corners);
    if (!deferred) { source.indices.resize(corners); }
    constexpr size_t blockSize = 65536;
    const auto blocks = (corners+blockSize-1)/blockSize;
    std::vector<uint64_t> firstBits((corners+63)/64);
    std::vector<uint32_t> counts(blocks), offsets(blocks), flags(blocks);
    std::vector<Table> shards(workers);
    const auto* preciseUvs = Precise ? buffers->texcoords.data() : nullptr;
    const auto* floatUvs = result.attributes.texcoords.data();
    const auto uvCount = Precise ? buffers->texcoords.size() : result.attributes.texcoords.size()/2;
    const bool finite = allUvsFinite<Precise>(preciseUvs,floatUvs,uvCount,progress,corners,totalIndices);
    auto* indices = geometry.indices.data();
    auto* primary = table.primary.data();
    const auto caller = std::this_thread::get_id();
    std::atomic<bool> stopped{false};
    std::atomic<size_t> completed{0};
    std::atomic<size_t> discovered{0};
    std::exception_ptr callbackError;
    const auto check = [&] {
        if (std::this_thread::get_id() == caller && !stopped.load(std::memory_order_relaxed)) {
            try { reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,
                completed.load(std::memory_order_relaxed),totalIndices); }
            catch (...) { callbackError = std::current_exception(); stopped.store(true,std::memory_order_relaxed); }
        }
    };
    const auto run = [&](size_t count, size_t batch, const auto& function) {
        parallelWorkBatches(count,batch,check,[&](size_t begin, size_t end) {
            if (stopped.load(std::memory_order_relaxed)) { return; }
            try { function(begin,end); }
            catch (...) { stopped.store(true,std::memory_order_relaxed); throw; }
        },1,static_cast<unsigned>(workers-1));
        if (callbackError) { std::rethrow_exception(callbackError); }
    };
    times.allocate = elapsed(start); start = Clock::now();
    // Each position range has one writer, including all its seam keys. Input
    // blocks remain in corner order. Their bounds avoid full scans per range.
    struct InputBlock { const rapidobj::Index* input; size_t offset, count; int low = 0, high = 0; };
    std::vector<InputBlock> inputBlocks;
    size_t shapeOffset = 0;
    for (const auto& shape : result.shapes) {
        for (size_t i = 0; i < shape.mesh.indices.size(); i += discoverySize) {
            inputBlocks.push_back({shape.mesh.indices.data()+i,shapeOffset+i,std::min(discoverySize,shape.mesh.indices.size()-i)});
        }
        shapeOffset += shape.mesh.indices.size();
    }
    run(inputBlocks.size(),16,[&](size_t begin, size_t end) {
        for (size_t b = begin; b < end; ++b) {
            auto& block = inputBlocks[b];
            int low = std::numeric_limits<int>::max(), high = -1;
            for (size_t i = 0; i < block.count; ++i) {
                low = std::min(low,block.input[i].position_index); high = std::max(high,block.input[i].position_index);
            }
            if (low < 0 || static_cast<size_t>(high) >= positions) {
                throw std::runtime_error("OBJ contains an invalid source position index.");
            }
            block.low = low; block.high = high;
        }
    });
    for (const auto& block : inputBlocks) {
        for (size_t shard = 0; shard < workers; ++shard) {
            if (static_cast<size_t>(block.high) >= positions*shard/workers && static_cast<size_t>(block.low) < positions*(shard+1)/workers) {
                times.scanned += block.count;
            }
        }
    }
    times.scratchBytes = firstBits.capacity()*sizeof(uint64_t)
        + (counts.capacity()+offsets.capacity()+flags.capacity())*sizeof(uint32_t)
        + inputBlocks.capacity()*sizeof(InputBlock) + shards.capacity()*sizeof(Table);
    times.bounds = elapsed(start); start = Clock::now();
    run(workers,1,[&](size_t begin, size_t end) {
        for (size_t shard = begin; shard < end; ++shard) {
            const auto low = positions*shard/workers, high = positions*(shard+1)/workers;
            auto& seams = shards[shard];
            for (const auto& block : inputBlocks) {
                if (static_cast<size_t>(block.high) < low || static_cast<size_t>(block.low) >= high) { continue; }
                const auto* input = block.input;
                const auto count = block.count;
                const auto offset = block.offset;
                const auto previous = seams.next;
                for (size_t batch = 0; batch < count; batch += blockSize) {
                    check();
                    if (stopped.load(std::memory_order_relaxed)) { return; }
                    const auto last = std::min(count,batch+blockSize);
                    for (size_t i = batch; i < last; ++i) {
                        const auto& index = input[i];
                        const auto position = static_cast<size_t>(index.position_index);
                        if (position < low || position >= high) { continue; }
                        const Key key{index.position_index,index.normal_index,index.texcoord_index};
                        auto& entry = primary[position];
                        uint32_t first;
                        if (entry.id == empty) {
                            first = tagId<Precise>(key,static_cast<uint32_t>(offset+i),preciseUvs,floatUvs,finite);
                            entry = {first,key.normal,key.uv}; ++seams.next;
                        } else if (entry.normal == key.normal && entry.uv == key.uv) { first = entry.id; }
                        else { first = discoverSeam<Precise>(seams,key,static_cast<uint32_t>(offset+i),preciseUvs,floatUvs,finite); }
                        indices[offset+i] = first;
                        if (!deferred) { source.indices[offset+i] = static_cast<uint32_t>(position); }
                    }
                }
                const auto added = seams.next-previous;
                if (added) {
                    (void)sceneBufferBytes(discovered.fetch_add(added,std::memory_order_relaxed)+added,sizeof(Vertex));
                }
            }
        }
    });
    times.discover = elapsed(start); start = Clock::now();
    // Mark first uses before overwriting any temporary offsets. Block counts
    // give the same ID sequence as the serial first-use traversal.
    run(blocks,1,[&](size_t begin, size_t end) {
        for (size_t block = begin; block < end; ++block) {
            const auto last = std::min(corners,(block+1)*blockSize);
            uint32_t unique = 0, uvFlags = 0;
            for (size_t word = block*blockSize; word < last; word += 64) {
                uint64_t bits = 0;
                const auto wordEnd = std::min(last,word+64);
                size_t i = word;
#if defined(__AVX2__)
                auto expected = _mm256_add_epi32(_mm256_set1_epi32(static_cast<int>(word)),
                    _mm256_setr_epi32(0,1,2,3,4,5,6,7));
                auto uvBits = _mm256_setzero_si256();
                for (; i+8 <= wordEnd; i += 8) {
                    const auto values = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(indices+i));
                    uvBits = _mm256_or_si256(uvBits,values);
                    const auto first = _mm256_and_si256(values,_mm256_set1_epi32(static_cast<int>(idMask)));
                    const auto matches = _mm256_cmpeq_epi32(first,expected);
                    bits |= uint64_t(static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(matches)))) << (i-word);
                    expected = _mm256_add_epi32(expected,_mm256_set1_epi32(8));
                }
                if (_mm256_movemask_ps(_mm256_castsi256_ps(uvBits))) { uvFlags |= invalidUv; }
#endif
                for (; i < wordEnd; ++i) {
                    const auto first = indices[i]; uvFlags |= first;
                    if ((first & idMask) == i) { bits |= uint64_t{1} << (i-word); }
                }
                unique += static_cast<uint32_t>(std::popcount(bits));
                firstBits[word/64] = bits;
            }
            counts[block] = unique; flags[block] = uvFlags;
        }
    });
    for (size_t block = 0; block < blocks; ++block) {
        offsets[block] = table.next; table.next += counts[block];
    }
    (void)sceneBufferBytes(table.next,sizeof(Vertex));
    size_t offset = 0;
    for (size_t shapeIndex = 0; shapeIndex < result.shapes.size(); ++shapeIndex) {
        const auto& shape = result.shapes[shapeIndex];
        const auto count = shape.mesh.indices.size();
        if (!count) { continue; }
        uint32_t uvFlags = 0;
        // Boundary fragments retain the UV flags from discovery.
        size_t i = offset;
        for (; i < offset+count && i%blockSize; ++i) { uvFlags |= indices[i]; }
        for (; i+blockSize <= offset+count; i += blockSize) { uvFlags |= flags[i/blockSize]; }
        for (; i < offset+count; ++i) { uvFlags |= indices[i]; }
        MeshNode node; node.name = shape.name.empty() ? "shape "+std::to_string(shapeIndex+1) : shape.name;
        node.indexOffset = static_cast<uint32_t>(offset); node.indexCount = static_cast<uint32_t>(count);
        node.hasTexcoords = !(uvFlags & invalidUv); geometry.nodes.push_back(std::move(node));
        offset += count;
    }
    times.count = elapsed(start); start = Clock::now();
    run(blocks,1,[&](size_t begin, size_t end) {
        for (size_t block = begin; block < end; ++block) {
            uint32_t id = offsets[block];
            const auto last = std::min(firstBits.size(),(block+1)*blockSize/64);
            for (size_t word = block*blockSize/64; word < last; ++word) {
                auto bits = firstBits[word];
                while (bits) {
                    const auto corner = word*64+static_cast<size_t>(std::countr_zero(bits));
                    indices[corner] = id++;
                    bits &= bits-1;
                }
            }
        }
    });
    // First-corner slots now hold final IDs. All other slots still refer to
    // first corners. Keep those final slots unchanged during parallel output.
    run(positions,blockSize,[&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            auto& entry = primary[i];
            if (entry.id != empty) { entry.id = (entry.id & invalidUv) | indices[entry.id & idMask]; }
        }
    });
    times.assign = elapsed(start); start = Clock::now();
    size_t seamCount = 0;
    for (const auto& shard : shards) { seamCount += shard.seams.size(); }
    table.seams.reserve(seamCount);
    for (const auto& shard : shards) {
        times.scratchBytes += shard.seams.capacity()*sizeof(Seam) + shard.buckets.capacity()*sizeof(uint32_t);
    }
    for (auto& shard : shards) {
        check();
        if (callbackError) { std::rethrow_exception(callbackError); }
        for (auto seam : shard.seams) {
            seam.id = (seam.id & invalidUv) | indices[seam.id & idMask]; table.seams.push_back(seam);
        }
        shard = {};
    }
    std::sort(table.seams.begin(),table.seams.end(),[](const Seam& a, const Seam& b) { return (a.id & idMask) < (b.id & idMask); });
    if (seamCount) {
        table.buckets.assign(std::bit_ceil(std::max(size_t{16},seamCount*4/3+1)),empty);
        for (size_t slot = 0; slot < seamCount; ++slot) {
            auto bucket = hashKey(table.seams[slot].key) & (table.buckets.size()-1);
            while (table.buckets[bucket] != empty) { bucket = (bucket+1) & (table.buckets.size()-1); }
            table.buckets[bucket] = static_cast<uint32_t>(slot);
        }
    }
    times.merge = elapsed(start); start = Clock::now();
    run(blocks,1,[&](size_t begin, size_t end) {
        for (size_t block = begin; block < end; ++block) {
            const auto last = std::min(corners,(block+1)*blockSize);
            for (size_t word = block*blockSize; word < last; word += 64) {
                auto bits = ~firstBits[word/64];
                if (last-word < 64) { bits &= (uint64_t{1} << (last-word))-1; }
                while (bits) {
                    const auto i = word+static_cast<size_t>(std::countr_zero(bits));
                    indices[i] = indices[indices[i] & idMask];
                    bits &= bits-1;
                }
            }
            completed.fetch_add(last-block*blockSize,std::memory_order_relaxed);
        }
    });
    times.output = elapsed(start);
    return times;
}
}
