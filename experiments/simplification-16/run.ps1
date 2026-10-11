param(
    [string]$Executable = "$PSScriptRoot/../../build/vs2026-vcpkg/bin/Release/woby_simplification_16_probe.exe",
    [string]$Model = 'D:/temp/obj_tests/uploads_files_2720101_BusGameMap.obj',
    [string]$LargeModel = 'D:/temp/obj_tests/powerplant.obj',
    [int]$Samples = 5,
    [string]$Output = "$PSScriptRoot/../../doc/simplification-16-results.json"
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/provenance.ps1"
$repositoryRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$workloads = @(
    @{ Path = $Model; Samples = $Samples },
    @{ Path = "$PSScriptRoot/../../assets/samples/surface-mesh-quality/topology.obj"; Samples = $Samples },
    @{ Path = "$PSScriptRoot/../../assets/samples/fin-candidates/attached-fin.obj"; Samples = $Samples },
    @{ Path = $LargeModel; Samples = 3 }
)
$results = @()
$models = @()
foreach ($workload in $workloads) {
    $path = $workload.Path
    $raw = & $Executable $path "$($workload.Samples)"
    if ($LASTEXITCODE -ne 0) { throw "Probe failed for $path" }
    $results += ($raw -join "`n" | ConvertFrom-Json)
    $models += [ordered]@{ path = $path; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$metadata = [ordered]@{
    executable_sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
    configuration = 'Release'
    probe_source_sha256 = (Get-FileHash -LiteralPath "$PSScriptRoot/main.cpp" -Algorithm SHA256).Hash.ToLowerInvariant()
    provenance = Get-SimplificationProvenance -RepositoryRoot $repositoryRoot -Paths @(
        'CMakeLists.txt', 'experiments/simplification-16/CMakeLists.txt',
        'experiments/simplification-16/main.cpp', 'experiments/simplification-16/run.ps1',
        'experiments/simplification-16/allocation_probe.h', 'experiments/simplification-16/provenance.ps1',
        'src/mesh_topology.h', 'src/mesh_topology.cpp', 'src/obj_mesh.h', 'src/obj_mesh.cpp',
        'src/model_mesh.h', 'src/model_mesh.cpp'
    )
    models = $models
    runs = $results
}
$metadata | ConvertTo-Json -Depth 80 | Set-Content -LiteralPath $Output -Encoding utf8
