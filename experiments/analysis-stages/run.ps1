param(
    [Parameter(Mandatory)][string]$Model,
    [ValidateRange(1,5)][int]$Samples = 3,
    [string]$Executable = (Join-Path $PSScriptRoot '../../build/vs2026-vcpkg/bin/Release/woby_analysis_stages_probe.exe'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../build/analysis-stages/results')
)
$ErrorActionPreference = 'Stop'
$modelPath = (Resolve-Path -LiteralPath $Model).Path
$resultPath = [IO.Path]::GetFullPath($OutputDirectory)
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
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
    notes = 'One process, samples in fixed order; no warmup excluded. One source batch, all parts, identity transforms. Default intersection budgets. No concurrent builds or tests. Production implementation with cold geometry, warm reuse, topology upgrade and threshold display timings.'
}
$baseline = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../../doc/analysis-stage-separation-results.json') -Raw | ConvertFrom-Json
if ($metadata.executable_sha256 -eq $baseline.metadata.executable_sha256) {
    $metadata.source_sha256 = $baseline.metadata.source_sha256
    $metadata.notes = $baseline.metadata.notes + ' Repeated baseline executable; hash checked against the initial investigation.'
}
$metadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $resultPath 'metadata.json')
$process = Start-Process -FilePath $executablePath -ArgumentList @(('"' + $modelPath + '"'), $Samples) -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $resultPath 'run.json') -RedirectStandardError (Join-Path $resultPath 'run.log')
$peakWorkingSet = 0L
while (-not $process.WaitForExit(200)) {
    $process.Refresh()
    $peakWorkingSet = [Math]::Max($peakWorkingSet, $process.PeakWorkingSet64)
}
$metadata.peak_working_set_bytes = $peakWorkingSet
$metadata.peak_memory_scope = 'Windows PeakWorkingSet64 sampled every 200 ms for the whole probe process. Includes loader, retained full topology, comparisons and all benchmark paths; not an isolated operation peak or private committed memory.'
$metadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $resultPath 'metadata.json')
if ($process.ExitCode -ne 0) { throw 'Probe failed; see run.log' }
