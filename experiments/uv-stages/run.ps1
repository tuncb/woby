param(
    [Parameter(Mandatory)][string]$Model,
    [ValidateRange(1,5)][int]$Samples = 3,
    [string]$Executable = (Join-Path $PSScriptRoot '../../build/vs2026-vcpkg/bin/Release/woby_uv_stages_probe.exe'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../build/uv-stages/results')
)
$ErrorActionPreference = 'Stop'
$modelPath = (Resolve-Path -LiteralPath $Model).Path
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$resultPath = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $resultPath | Out-Null
$metadata = [ordered]@{
    base_commit = (& git -C $PSScriptRoot rev-parse HEAD)
    model = $modelPath
    model_sha256 = (Get-FileHash -LiteralPath $modelPath).Hash
    executable_sha256 = (Get-FileHash -LiteralPath $executablePath).Hash
    source_sha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'main.cpp')).Hash
    date = (Get-Date -Format 'yyyy-MM-dd')
    cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    samples = $Samples
    notes = 'Release CPU timings in one process after load. All parts, identity transforms, surface angle per-patch normalization. Fixed operation order; no warmup excluded. Overlap off except the separate enable-overlap timing, which uses default limits. No concurrent builds or tests. layout_from_surface times exclude face packing and smooth normals; prepare_*_cached times include full CPU preparation with shared measurements. All timings exclude GPU upload and runtime publication. Surface and cold layout remain resident during cached layout timing. Array capacities exclude allocator overhead and scratch storage.'
}
$metadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $resultPath 'metadata.json')
$process = Start-Process -FilePath $executablePath -ArgumentList @(('"' + $modelPath + '"'), $Samples) -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $resultPath 'run.json') -RedirectStandardError (Join-Path $resultPath 'run.log')
$process.WaitForExit()
if ($process.ExitCode -ne 0) { throw 'Probe failed; see run.log' }
