param(
    [string]$Model,
    [ValidateRange(1,20)][int]$Runs=5,
    [string]$Executable=(Join-Path $PSScriptRoot '../../build/vs2026-vcpkg/bin/Release/woby_simplification_10_probe.exe'),
    [string]$OutputDirectory=(Join-Path $PSScriptRoot '../../build/simplification-10/results')
)
$ErrorActionPreference='Stop'
$executablePath=(Resolve-Path -LiteralPath $Executable).Path
$outputPath=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$sourceHashes=[ordered]@{}
foreach($name in @('main.cpp','predicate.cpp','adaptive_predicates.h','probe.h','CMakeLists.txt','run.ps1')) {
    $sourceHashes[$name]=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $name)).Hash
}
$metadata=[ordered]@{
    base_commit=(& git -C $PSScriptRoot rev-parse HEAD)
    date=(Get-Date -Format 'yyyy-MM-dd')
    cpu=(Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    executable_sha256=(Get-FileHash -LiteralPath $executablePath).Hash
    production_source_sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot '../../src/mesh_intersections.cpp')).Hash
    installed_predicates_sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot '../../build/vs2026-vcpkg/vcpkg_installed/x64-windows/include/predicates.h')).Hash
    sources=$sourceHashes
    runs=$Runs
    allocation_scope='C++ global new/new[] requested payload only. Excludes malloc internals, allocator headers, aligned new, DLL-internal allocations, source geometry and caller input. Timings use the same replacement allocator with recording disabled.'
}
$argument='--synthetic'
$label='synthetic'
if($Model) {
    $modelPath=(Resolve-Path -LiteralPath $Model).Path
    $metadata['model']=$modelPath
    $metadata['model_sha256']=(Get-FileHash -LiteralPath $modelPath).Hash
    $argument='"'+$modelPath+'"'
    $label='model'
}
$metadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $outputPath "$label-metadata.json")
$process=Start-Process -FilePath $executablePath -ArgumentList @($argument,$Runs) -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $outputPath "$label.json") -RedirectStandardError (Join-Path $outputPath "$label.log")
$process.WaitForExit()
if($process.ExitCode -ne 0) { throw "Probe failed with exit code $($process.ExitCode). See $label.log." }
