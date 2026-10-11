param([string]$Model = 'D:/temp/obj_tests/uploads_files_2720101_BusGameMap.obj',
      [string]$BinaryDirectory = (Join-Path $PSScriptRoot '../../build/vs2026-vcpkg/bin/Release'),
      [string]$Output = (Join-Path $PSScriptRoot '../../doc/simplification-13-results.json'))
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Output) { throw "Output already exists: $Output" }
$exe = Join-Path (Resolve-Path -LiteralPath $BinaryDirectory).Path 'woby_instance_query_probe.exe'
$modelPath = (Resolve-Path -LiteralPath $Model).Path
# Read the input before timing. These measurements use a warm filesystem cache.
$inputHash = (Get-FileHash -LiteralPath $modelPath -Algorithm SHA256).Hash
$synthetic = (& $exe | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0) { throw 'Synthetic instance probe failed.' }
$modelResult = (& $exe $modelPath | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0) { throw 'Real-model instance probe failed.' }
$hashes = [ordered]@{}
foreach ($name in @('main.cpp','CMakeLists.txt','run.ps1','../../src/mesh_comparison.cpp','../../src/comparison_scene.cpp')) {
    $hashes[$name] = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $name)).Hash
}
[ordered]@{
    base_commit = (& git -C $PSScriptRoot rev-parse HEAD)
    date = (Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz')
    cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    executable_sha256 = (Get-FileHash -LiteralPath $exe).Hash
    sources = $hashes
    model_sha256 = $inputHash
    model_input_bytes = (Get-Item -LiteralPath $modelPath).Length
    synthetic = $synthetic
    model = $modelResult
} | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $Output -Encoding utf8
