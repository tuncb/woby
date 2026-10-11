param(
    [Parameter(Mandatory = $true)][string]$BaselineBinary,
    [string]$CandidateBinary = "$PSScriptRoot/../../build/vs2026-vcpkg/bin/Release/woby_annotation_history_benchmark.exe",
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateRange(1,9)][int]$Samples = 5,
    [ValidateRange(1,9)][int]$SamplesPerProcess = 1,
    [switch]$MetadataOnly,
    [ValidateRange(0,9223372036854775807)][long]$ProcessorAffinity = 0
)
$ErrorActionPreference = 'Stop'
$taskBaseline = (Resolve-Path -LiteralPath $BaselineBinary).Path
$taskCandidate = (Resolve-Path -LiteralPath $CandidateBinary).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory.' }
$taskOutput = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$taskStarted = [DateTime]::UtcNow.ToString('o')
$taskBaselineHash = (Get-FileHash -LiteralPath $taskBaseline -Algorithm SHA256).Hash.ToLowerInvariant()
$taskCandidateHash = (Get-FileHash -LiteralPath $taskCandidate -Algorithm SHA256).Hash.ToLowerInvariant()
$taskRows = [System.Collections.Generic.List[object]]::new()
$taskCases = if ($MetadataOnly) { @(@(0,256), @(0,16384)) } else { @(@(147,1), @(20000,1), @(1000000,1), @(0,256), @(0,16384)) }
foreach ($taskCase in $taskCases) {
    for ($taskRun = 0; $taskRun -lt $Samples; ++$taskRun) {
        $taskOrder = if ($taskRun % 2 -eq 0) { @('baseline','candidate') } else { @('candidate','baseline') }
        foreach ($taskVariant in $taskOrder) {
            $taskBinary = if ($taskVariant -eq 'baseline') { $taskBaseline } else { $taskCandidate }
            $taskName = "$taskVariant-$($taskCase[0])-$($taskCase[1])-$taskRun.json"
            $taskPath = Join-Path $taskOutput $taskName
            if ($ProcessorAffinity) {
                $taskProcess = Start-Process -FilePath $taskBinary -ArgumentList @($taskCase[0],$taskCase[1],$SamplesPerProcess) -PassThru -WindowStyle Hidden -RedirectStandardOutput $taskPath -RedirectStandardError "$taskPath.stderr.txt"
                try {
                    $taskProcess.ProcessorAffinity = [IntPtr]$ProcessorAffinity
                    $taskProcess.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::AboveNormal
                    $taskProcess.WaitForExit()
                    if ($taskProcess.ExitCode -ne 0) { throw "Benchmark failed: $taskName" }
                } finally { $taskProcess.Dispose() }
            } else {
                & $taskBinary $taskCase[0] $taskCase[1] $SamplesPerProcess > $taskPath
                if ($LASTEXITCODE -ne 0) { throw "Benchmark failed: $taskName" }
            }
            $taskData = Get-Content -LiteralPath $taskPath -Raw | ConvertFrom-Json
            foreach ($taskMode in @('discrete','drag')) {
                for ($taskSampleIndex = 0; $taskSampleIndex -lt $SamplesPerProcess; ++$taskSampleIndex) {
                    $taskSample = $taskData.$taskMode[$taskSampleIndex]
                    if (-not $taskSample.verified) { throw "Unverified sample: $taskName" }
                    $taskRows.Add([ordered]@{
                        variant=$taskVariant; segments=$taskCase[0]; groups=$taskCase[1]; run=$taskRun
                        sample=$taskSampleIndex; order=($taskOrder -join ','); mode=$taskMode; raw=$taskName; measurement=$taskSample
                    })
                }
            }
            Write-Host "$taskVariant segments=$($taskCase[0]) groups=$($taskCase[1]) run=$taskRun passed"
        }
    }
}
if ((Get-FileHash -LiteralPath $taskBaseline -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskBaselineHash -or
    (Get-FileHash -LiteralPath $taskCandidate -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskCandidateHash) {
    throw 'A benchmark binary changed during the run.'
}
[ordered]@{
    started_utc=$taskStarted; finished_utc=[DateTime]::UtcNow.ToString('o')
    baseline_binary=$taskBaseline; baseline_sha256=$taskBaselineHash
    candidate_binary=$taskCandidate; candidate_sha256=$taskCandidateHash
    benchmark_source_sha256=(Get-FileHash -LiteralPath "$PSScriptRoot/production.cpp" -Algorithm SHA256).Hash.ToLowerInvariant()
    base_commit=(git -C "$PSScriptRoot/../.." rev-parse HEAD).Trim()
    samples=$Samples; samples_per_process=$SamplesPerProcess
    processor_affinity=$ProcessorAffinity
    priority=$(if ($ProcessorAffinity) { 'AboveNormal' } else { 'Inherited' })
    scope='Release: 12 production property edits, each followed by history recording and a dirty query. Fixture, initial history, serialization and renderer excluded. Requested allocation counters cover all C++ allocations in this process. Unique geometry payload includes live state, clean document and all retained history records.'
    rows=$taskRows
} | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $taskOutput 'results.json') -Encoding utf8NoBOM
