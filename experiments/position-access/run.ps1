param(
    [Parameter(Mandatory)][string]$Model,
    [string]$BinaryDirectory = (Join-Path $PSScriptRoot '../../build/vs2026-vcpkg/bin/Release'),
    [string]$BaselineDirectory = (Join-Path $PSScriptRoot '../../build/position-storage-baseline'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../build/position-access-model'),
    [ValidateRange(1, 9)][int]$Samples = 3,
    [ValidateRange(1, 5)][int]$Runs = 3
)
$ErrorActionPreference = 'Stop'
$modelPath = (Resolve-Path -LiteralPath $Model).Path
$binaryPath = (Resolve-Path -LiteralPath $BinaryDirectory).Path
$resultPath = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $resultPath | Out-Null
$metadata = [ordered]@{
    commit = (& git -C $PSScriptRoot rev-parse HEAD)
    model = $modelPath
    model_bytes = (Get-Item -LiteralPath $modelPath).Length
    model_sha256 = (Get-FileHash -LiteralPath $modelPath).Hash
    date = (Get-Date -Format 'yyyy-MM-dd')
    samples = $Samples
    warmup_runs = 1
    runs = $Runs
    cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    physical_memory_bytes = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
    memory_sample_interval_ms = 20
    processes = @()
}
$expectedModel = $null
$expectedOperations = @{}
foreach ($run in 1..$Runs) {
    $variants = if ($run % 2) { @('before', 'production') } else { @('production', 'before') }
    foreach ($variant in $variants) {
        $variantPath = if ($variant -eq 'before') { (Resolve-Path -LiteralPath $BaselineDirectory).Path } else { $binaryPath }
        $executable = Join-Path $variantPath "woby_position_$variant.exe"
        $stdout = Join-Path $resultPath "$variant-$run.json"
        $stderr = Join-Path $resultPath "$variant-$run.log"
        Write-Output "Starting $variant run $run"
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $process = Start-Process -FilePath $executable -ArgumentList @(('"' + $modelPath + '"'), $Samples) `
            -WorkingDirectory $variantPath -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $peakPrivate = 0L
        $peakWorking = 0L
        while (-not $process.HasExited) {
            $process.Refresh()
            $peakPrivate = [Math]::Max($peakPrivate, $process.PrivateMemorySize64)
            $peakWorking = [Math]::Max($peakWorking, $process.PeakWorkingSet64)
            Start-Sleep -Milliseconds 20
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "$variant failed; see $stderr" }
        $result = Get-Content -LiteralPath $stdout -Raw | ConvertFrom-Json
        $modelDigest = $result.model | ConvertTo-Json -Depth 30 -Compress
        if ($null -eq $expectedModel) { $expectedModel = $modelDigest }
        if ($expectedModel -cne $modelDigest) { throw 'Input geometry or source relationship changed' }
        if (@($result.operations.PSObject.Properties).Count -ne 4) { throw 'Incomplete workload result' }
        foreach ($operation in $result.operations.PSObject.Properties) {
            $digest = $operation.Value.output | ConvertTo-Json -Depth 30 -Compress
            if ($expectedOperations.ContainsKey($operation.Name) -and $expectedOperations[$operation.Name] -cne $digest) {
                throw "Output changed: $($operation.Name)"
            }
            $expectedOperations[$operation.Name] = $digest
        }
        $metadata.processes += [ordered]@{
            variant = $variant; run = $run
            wall_seconds = $clock.Elapsed.TotalSeconds
            sampled_peak_private_bytes = $peakPrivate
            peak_working_set_bytes = $peakWorking
            executable_sha256 = (Get-FileHash -LiteralPath $executable).Hash
        }
        $metadata | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $resultPath 'metadata.json')
        Write-Output "Completed $variant run $run in $([Math]::Round($clock.Elapsed.TotalSeconds, 1)) seconds"
    }
}
