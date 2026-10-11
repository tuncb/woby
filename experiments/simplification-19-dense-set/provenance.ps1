function Get-DenseSetSourceHashes {
    param([string]$RepositoryRoot, [string]$BuildDirectory)
    $paths = @(
        'CMakeLists.txt', 'experiments/simplification-19-dense-set/CMakeLists.txt',
        'experiments/simplification-19-dense-set/main.cpp',
        'experiments/simplification-19-dense-set/dense_set_index.h',
        'experiments/simplification-19-dense-set/dense_set_pipeline_index.h',
        'experiments/simplification-19-dense-set/tracked_custom_pipeline_index.h',
        'experiments/simplification-19-dense-set/index_allocation.h',
        'experiments/simplification-19-dense-set/run.ps1',
        'experiments/simplification-19-dense-set/exit-report-contract.ps1',
        'experiments/simplification-19-dense-set/provenance.ps1',
        'src/analysis_index.h', 'tests/performance_benchmarks.cpp', 'tests/analysis_export_benchmarks.cpp',
        'src/analysis_results.cpp', 'src/mesh_comparison.cpp', 'src/mesh_duplicates.cpp', 'src/mesh_topology.cpp',
        'src/model_mesh.cpp', 'src/obj_mesh.cpp', 'src/obj_freeform.cpp', 'src/freeform.cpp', 'src/freeform_trim.cpp',
        'src/mesh_degenerates.cpp', 'src/uv_quality.cpp', 'src/uv_overlap.cpp',
        'src/mesh_intersections.cpp', 'src/surface_mesh_quality.cpp')
    $hashes = [ordered]@{}
    foreach ($path in $paths) {
        $hashes[$path] = (Get-FileHash -LiteralPath (Join-Path $RepositoryRoot $path) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $anchor = '#include "analysis_index.h"'
    foreach ($module in @('mesh_comparison','mesh_duplicates','mesh_topology')) {
        $original = Get-Content -Raw -LiteralPath (Join-Path $RepositoryRoot "src/$module.cpp")
        $first = $original.IndexOf($anchor, [StringComparison]::Ordinal)
        if ($first -lt 0 -or $original.IndexOf($anchor, $first + $anchor.Length, [StringComparison]::Ordinal) -ge 0) {
            throw "Production include anchor changed: $module"
        }
        foreach ($backend in @('custom','dense')) {
            $replacement = if ($backend -eq 'custom') { '#include "tracked_custom_pipeline_index.h"' } else { '#include "dense_set_pipeline_index.h"' }
            $generatedPath = Join-Path $BuildDirectory "experiments/simplification-19-dense-set/generated/$backend/$module.cpp"
            $generated = Get-Content -Raw -LiteralPath $generatedPath
            $expected = $original.Replace($anchor,$replacement).Replace("`r`n","`n")
            if ($generated.Replace("`r`n","`n") -ne $expected) { throw "Generated source differs beyond its include: $backend $module" }
            $hashes["generated/$backend/$module.cpp"] = (Get-FileHash -LiteralPath $generatedPath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $custom = Get-Content -Raw -LiteralPath (Join-Path $RepositoryRoot 'src/analysis_index.h')
    $substitutions = @(
        @('#pragma once', "#pragma once`n#include `"index_allocation.h`"", 1),
        @('struct AnalysisIndex {', 'struct TrackedCustomAnalysisIndex {', 1),
        @('std::vector<std::array<T, N>> keys;', 'std::vector<std::array<T, N>, DenseSetTrackingAllocator<std::array<T, N>>> keys;', 1),
        @('std::vector<size_t> buckets;', 'std::vector<size_t, DenseSetTrackingAllocator<size_t>> buckets;', 1),
        @('AnalysisIndex<T, N>', 'TrackedCustomAnalysisIndex<T, N>', 2),
        @('analysisKeyHash', 'trackedCustomKeyHash', 4))
    foreach ($substitution in $substitutions) {
        if ([regex]::Matches($custom,[regex]::Escape($substitution[0])).Count -ne $substitution[2]) { throw "Custom extraction anchor changed: $($substitution[0])" }
        $custom = $custom.Replace($substitution[0],$substitution[1])
    }
    $customPath = Join-Path $BuildDirectory 'experiments/simplification-19-dense-set/generated/tracked_custom_index.h'
    $generatedCustom = Get-Content -Raw -LiteralPath $customPath
    if ($generatedCustom.Replace("`r`n","`n") -ne $custom.Replace("`r`n","`n")) { throw 'Generated custom hash/probing header differs beyond checked substitutions' }
    $hashes['generated/tracked_custom_index.h'] = (Get-FileHash -LiteralPath $customPath -Algorithm SHA256).Hash.ToLowerInvariant()
    foreach ($name in @('unordered_dense.h','unordered_dense/stl.h')) {
        $dependency = Join-Path $BuildDirectory "vcpkg_installed/x64-windows/include/ankerl/$name"
        if (Test-Path -LiteralPath $dependency) {
            $hashes["dependency/ankerl/$name"] = (Get-FileHash -LiteralPath $dependency -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    foreach ($target in @('woby_analysis_index_dense_set_probe','woby_benchmarks_dense_set_custom','woby_benchmarks_dense_set')) {
        $project = Join-Path $BuildDirectory "experiments/simplification-19-dense-set/$target.vcxproj"
        if (Test-Path -LiteralPath $project) { $hashes["project/$target.vcxproj"] = (Get-FileHash -LiteralPath $project -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    return $hashes
}
