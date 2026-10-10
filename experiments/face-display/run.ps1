param(
    [Parameter(Mandatory)][string]$Model,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../build/face-display/results'),
    [ValidateRange(1, 5)][int]$Runs = 3,
    [ValidateRange(1, 15)][int]$Samples = 7
)
$ErrorActionPreference = 'Stop'
$modelPath = (Resolve-Path -LiteralPath $Model).Path
$resultPath = [IO.Path]::GetFullPath($OutputDirectory)
$executable = (Resolve-Path (Join-Path $PSScriptRoot '../../build/vs2026-vcpkg/bin/Release/woby_face_display_probe.exe')).Path
New-Item -ItemType Directory -Force -Path $resultPath | Out-Null
$metadata = [ordered]@{
    commit = (& git -C $PSScriptRoot rev-parse HEAD)
    model = $modelPath
    model_sha256 = (Get-FileHash -LiteralPath $modelPath).Hash
    executable_sha256 = (Get-FileHash -LiteralPath $executable).Hash
    source_sha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'main.cpp')).Hash
    date = (Get-Date -Format 'yyyy-MM-dd')
    cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    runs = $Runs
    samples = $Samples
    warmups = 1
    shuffle_seed = 1729
    notes = 'CPU pack and checksum fetch loops; no GPU timing. Both representations remain resident. Load and metric analysis are outside timings.'
}
$metadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $resultPath 'metadata.json')
$expected = $null
foreach ($run in 1..$Runs) {
    $stdout = Join-Path $resultPath "run-$run.json"
    $stderr = Join-Path $resultPath "run-$run.log"
    Write-Output "Starting face display probe $run"
    $process = Start-Process -FilePath $executable -ArgumentList @(('"' + $modelPath + '"'), $Samples) -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "Probe failed; see $stderr" }
    $result = Get-Content -LiteralPath $stdout -Raw | ConvertFrom-Json
    $digest = "$($result.faces):$($result.sequential_hash):$($result.shuffled_hash)"
    if ($null -ne $expected -and $digest -cne $expected) { throw 'Output changed between processes' }
    $expected = $digest
    Write-Output "Completed face display probe $run"
}
