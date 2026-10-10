param(
    [Parameter(Mandatory)][string]$Model,
    [string]$Assets = (Join-Path $PSScriptRoot '../../build/vs2026-vcpkg/bin/Debug/assets'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../build/face-display/gpu-results')
)
$ErrorActionPreference = 'Stop'
$modelPath = (Resolve-Path -LiteralPath $Model).Path
$assetPath = (Resolve-Path -LiteralPath $Assets).Path
$resultPath = [IO.Path]::GetFullPath($OutputDirectory)
$executable = (Resolve-Path (Join-Path $PSScriptRoot '../../build/vs2026-vcpkg/bin/Release/woby_face_display_gpu_probe.exe')).Path
New-Item -ItemType Directory -Force -Path $resultPath | Out-Null
$metadata = [ordered]@{
    parent_commit = (& git -C $PSScriptRoot rev-parse HEAD)
    model = $modelPath
    model_sha256 = (Get-FileHash -LiteralPath $modelPath).Hash
    executable_sha256 = (Get-FileHash -LiteralPath $executable).Hash
    source_sha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'gpu.cpp')).Hash
    shader_sha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot '../../shaders/native/woby.slang')).Hash
    date = (Get-Date -Format 'yyyy-MM-dd')
    cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion)
    notes = 'One Release process; four alternating blocks of expanded/compact draws. Each block has 12 warmup frames and 31 GPU timestamp samples. Normalized Bus geometry, fixed display values. GPU timings include clear and one draw, no readback. No concurrent GPU tests or benchmarks.'
}
$metadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $resultPath 'metadata.json')
$process = Start-Process -FilePath $executable -ArgumentList @(('"' + $assetPath + '"'),('"' + $modelPath + '"')) -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $resultPath 'run.json') -RedirectStandardError (Join-Path $resultPath 'run.log')
$process.WaitForExit()
if ($process.ExitCode -ne 0) { throw 'GPU probe failed; see run.log' }
