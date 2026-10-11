param(
    [string]$Executable = "$PSScriptRoot/../../build/vs2026-vcpkg/bin/Release/woby_simplification_16_probe.exe",
    [string]$Model = 'D:/temp/obj_tests/uploads_files_2720101_BusGameMap.obj',
    [string]$LargeModel = 'D:/temp/obj_tests/powerplant.obj',
    [int]$Samples = 3,
    [string]$Output = "$PSScriptRoot/../../doc/simplification-16-proved-results.json"
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/provenance.ps1"
$repositoryRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$preserved = Get-Content -Raw -LiteralPath "$PSScriptRoot/../../doc/simplification-16-checked-results.json" | ConvertFrom-Json
$results = @()
$models = @()
foreach ($workload in @(@{ Path = $Model; ProvedOnly = $true }, @{ Path = $LargeModel; ProvedOnly = $false })) {
    $path = $workload.Path
    $arguments = @($path, "$Samples")
    if ($workload.ProvedOnly) { $arguments += '--proved' }
    $raw = & $Executable @arguments
    if ($LASTEXITCODE -ne 0) { throw "Probe failed for $path" }
    $result = $raw -join "`n" | ConvertFrom-Json
    $prior = @($preserved.runs | Where-Object { [IO.Path]::GetFullPath($_.model) -eq [IO.Path]::GetFullPath($path) })
    $matchesPreserved = $false
    if ($prior.Count -eq 1) {
        foreach ($mode in @('original_index', 'exact_position')) {
            $expected = $prior[0].$mode[0]
            foreach ($sample in $result.$mode) {
                if ($sample.array_capacity_bytes.compact_proved_graph -ne $expected.array_capacity_bytes.compact_graph) {
                    throw "Proved graph capacity changed for $path $mode"
                }
                foreach ($field in @('geometry', 'wide_graph')) {
                    if ($sample.array_capacity_bytes.$field -ne $expected.array_capacity_bytes.$field) {
                        throw "Unchanged array capacity changed for $path $mode $field"
                    }
                }
                foreach ($property in $expected.counts.PSObject.Properties) {
                    if ($sample.counts.($property.Name) -ne $property.Value) {
                        throw "Graph cardinality changed for $path $mode $($property.Name)"
                    }
                }
            }
        }
        $matchesPreserved = $true
    }
    $result | Add-Member -NotePropertyName matches_preserved_checked_payload -NotePropertyValue $matchesPreserved
    $results += $result
    $models += [ordered]@{ path = $path; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$metadata = [ordered]@{
    executable_sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
    configuration = 'Release'
    probe_source_sha256 = (Get-FileHash -LiteralPath "$PSScriptRoot/main.cpp" -Algorithm SHA256).Hash.ToLowerInvariant()
    provenance = Get-SimplificationProvenance -RepositoryRoot $repositoryRoot -Paths @(
        'CMakeLists.txt', 'experiments/simplification-16/CMakeLists.txt',
        'experiments/simplification-16/main.cpp', 'experiments/simplification-16/checked-main.cpp',
        'experiments/simplification-16/run-proved.ps1', 'experiments/simplification-16/run.ps1',
        'experiments/simplification-16/allocation_probe.h', 'experiments/simplification-16/provenance.ps1',
        'doc/simplification-16-checked-results.json', 'src/mesh_topology.h', 'src/mesh_topology.cpp',
        'src/obj_mesh.h', 'src/obj_mesh.cpp', 'src/model_mesh.h', 'src/model_mesh.cpp'
    )
    models = $models
    runs = $results
}
$metadata | ConvertTo-Json -Depth 80 | Set-Content -LiteralPath $Output -Encoding utf8
