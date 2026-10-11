param(
    [string]$Executable = "$PSScriptRoot/../../build/vs2026-vcpkg/bin/Release/woby_simplification_11_probe.exe",
    [string]$Model = 'D:/temp/obj_tests/uploads_files_2720101_BusGameMap.obj',
    [int]$Samples = 5,
    [string]$Output = "$PSScriptRoot/../../doc/simplification-11-results.json"
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../simplification-16/provenance.ps1"
$repositoryRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$workloads = @(
    @('0', '256', '12', "$Samples"),
    @('20000', '1', '12', "$Samples"),
    @('1000000', '1', '12', "$Samples"),
    @('0', '16384', '12', "$Samples"),
    @('--scene', "$PSScriptRoot/../../assets/samples/surface-annotations/projected-ranges.woby", '12', "$Samples"),
    @('--model', $Model, '12', "$Samples")
)
$results = @()
foreach ($arguments in $workloads) {
    $raw = & $Executable @arguments
    if ($LASTEXITCODE -ne 0) { throw "Probe failed for $arguments" }
    $results += ($raw -join "`n" | ConvertFrom-Json)
}
$metadata = [ordered]@{
    executable_sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
    model_sha256 = (Get-FileHash -LiteralPath $Model -Algorithm SHA256).Hash.ToLowerInvariant()
    configuration = 'Release'
    probe_source_sha256 = (Get-FileHash -LiteralPath "$PSScriptRoot/main.cpp" -Algorithm SHA256).Hash.ToLowerInvariant()
    provenance = Get-SimplificationProvenance -RepositoryRoot $repositoryRoot -Paths @(
        'CMakeLists.txt', 'experiments/simplification-11/CMakeLists.txt',
        'experiments/simplification-11/main.cpp', 'experiments/simplification-11/run.ps1',
        'experiments/simplification-16/allocation_probe.h', 'experiments/simplification-16/provenance.ps1',
        'assets/samples/surface-annotations/projected-ranges.woby', 'assets/samples/surface-annotations/curved.obj',
        'src/scene_history.h', 'src/scene_history.cpp', 'src/scene_history_load.cpp',
        'src/scene_file.h', 'src/scene_file.cpp', 'src/ui_state.h', 'src/ui_state.cpp',
        'src/ui_operations.h', 'src/ui_operations.cpp', 'src/annotation_types.h',
        'src/ui_annotation_operations.cpp', 'src/obj_mesh.cpp', 'src/model_mesh.h', 'src/model_load.cpp'
    )
    runs = $results
}
$metadata | ConvertTo-Json -Depth 80 | Set-Content -LiteralPath $Output -Encoding utf8
