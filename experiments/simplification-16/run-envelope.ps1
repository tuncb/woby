param(
    [string]$Executable = "$PSScriptRoot/../../build/vs2026-vcpkg/bin/Release/woby_simplification_16_probe.exe",
    [string]$Model = 'D:/temp/obj_tests/powerplant.obj',
    [int]$Samples = 3,
    [string]$Output = "$PSScriptRoot/../../doc/simplification-16-envelope-results.json"
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/provenance.ps1"
$repositoryRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$priorFile = "$PSScriptRoot/../../doc/simplification-16-proved-results.json"
$prior = Get-Content -Raw -LiteralPath $priorFile | ConvertFrom-Json
$executableHash = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
$sourceHash = (Get-FileHash -LiteralPath "$PSScriptRoot/main.cpp" -Algorithm SHA256).Hash.ToLowerInvariant()
if ($executableHash -ne $prior.executable_sha256 -or $sourceHash -ne $prior.probe_source_sha256) {
    throw 'Envelope trial must use the same executable and source as the proved trial'
}
$started = [datetime]::UtcNow
$raw = & $Executable $Model "$Samples" '--proved'
$ended = [datetime]::UtcNow
if ($LASTEXITCODE -ne 0) { throw "Probe failed for $Model" }
$result = $raw -join "`n" | ConvertFrom-Json
$expectedRun = @($prior.runs | Where-Object { [IO.Path]::GetFullPath($_.model) -eq [IO.Path]::GetFullPath($Model) })
if ($expectedRun.Count -ne 1) { throw 'The envelope input must be present in the proved trial' }
foreach ($mode in @('original_index', 'exact_position')) {
    if (@($result.$mode).Count -ne $Samples) { throw "Wrong sample count for $mode" }
    $expected = $expectedRun[0].$mode[0]
    foreach ($sample in $result.$mode) {
        if (-not $sample.verified) { throw "Graph verification failed for $mode" }
        foreach ($field in @('geometry', 'wide_graph', 'compact_proved_graph')) {
            if ($sample.array_capacity_bytes.$field -ne $expected.array_capacity_bytes.$field) {
                throw "Array capacity changed for $mode $field"
            }
        }
        foreach ($property in $expected.counts.PSObject.Properties) {
            if ($sample.counts.($property.Name) -ne $property.Value) { throw "Graph cardinality changed for $mode $($property.Name)" }
        }
    }
}
$metadata = [ordered]@{
    executable_sha256 = $executableHash
    probe_source_sha256 = $sourceHash
    configuration = 'Release'
    started_utc = $started.ToString('o')
    ended_utc = $ended.ToString('o')
    timing_envelope = 'Production and proved compact retained; same executable as the all-four trial'
    matches_prior_executable_source_counts_and_capacities = $true
    model = [ordered]@{ path = $Model; sha256 = (Get-FileHash -LiteralPath $Model -Algorithm SHA256).Hash.ToLowerInvariant() }
    provenance = Get-SimplificationProvenance -RepositoryRoot $repositoryRoot -CompletedAt $ended -Paths @(
        'CMakeLists.txt', 'experiments/simplification-16/CMakeLists.txt',
        'experiments/simplification-16/main.cpp', 'experiments/simplification-16/checked-main.cpp',
        'experiments/simplification-16/run-envelope.ps1', 'experiments/simplification-16/run-proved.ps1',
        'experiments/simplification-16/allocation_probe.h', 'experiments/simplification-16/provenance.ps1',
        'doc/simplification-16-proved-results.json', 'src/mesh_topology.h', 'src/mesh_topology.cpp',
        'src/obj_mesh.h', 'src/obj_mesh.cpp', 'src/model_mesh.h', 'src/model_mesh.cpp'
    )
    runs = @($result)
}
$metadata | ConvertTo-Json -Depth 80 | Set-Content -LiteralPath $Output -Encoding utf8
