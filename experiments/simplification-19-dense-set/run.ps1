param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../../build/vs2026-vcpkg'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../doc/benchmarks/simplification-19-dense-set'),
    [string]$Output = (Join-Path $PSScriptRoot '../../doc/simplification-19-dense-set-results.json'),
    [string[]]$Models = @('D:/temp/obj_tests/uploads_files_2720101_BusGameMap.obj', 'D:/temp/obj_tests/powerplant.obj')
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/provenance.ps1"
if ((Test-Path -LiteralPath $Output) -or (Test-Path -LiteralPath $OutputDirectory)) { throw 'Output already exists.' }
$repositoryRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$buildPath = (Resolve-Path -LiteralPath $BuildDirectory).Path
$binaries = [ordered]@{
    micro = Join-Path $buildPath 'bin/Release/woby_analysis_index_dense_set_probe.exe'
    custom = Join-Path $buildPath 'bin/Release/woby_benchmarks_dense_set_custom.exe'
    dense_set = Join-Path $buildPath 'bin/Release/woby_benchmarks_dense_set.exe'
}
$binaryHashes = [ordered]@{}
foreach ($backend in $binaries.Keys) { $binaryHashes[$backend] = (Get-FileHash -LiteralPath $binaries[$backend] -Algorithm SHA256).Hash.ToLowerInvariant() }
$sourceHashes = Get-DenseSetSourceHashes -RepositoryRoot $repositoryRoot -BuildDirectory $buildPath
$started = [datetime]::UtcNow
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputPath | Out-Null
$micro = (& $binaries.micro | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0 -or -not $micro.contracts.ok) { throw 'Dense-set microbenchmark failed.' }
$records = @()
$expectedCounts = @{}
$expectedLayout = @{}
foreach ($model in $Models) {
    $modelPath = (Resolve-Path -LiteralPath $model).Path
    $hash = (Get-FileHash -LiteralPath $modelPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $label = [IO.Path]::GetFileNameWithoutExtension($modelPath)
    foreach ($order in 0..1) {
        $backends = if ($order -eq 0) { @('custom','dense_set') } else { @('dense_set','custom') }
        foreach ($backend in $backends) {
            $csv = Join-Path $outputPath "$label-$order-$backend.csv"
            $log = Join-Path $outputPath "$label-$order-$backend.log"
            $process = Start-Process -FilePath $binaries[$backend] `
                -ArgumentList @('detectors', ('"' + $modelPath + '"'), '3') -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput $csv -RedirectStandardError $log
            $process.WaitForExit()
            if ($process.ExitCode -ne 0) { throw "Detector probe failed: $backend $modelPath. See $log." }
            $allocationLines = @(Get-Content -LiteralPath $log | Where-Object { $_ -match '^\{"analysis_index_allocations":' })
            if ($allocationLines.Count -ne 1) { throw "Expected one analysis-index allocation summary: $log" }
            $indexAllocation = ($allocationLines[0] | ConvertFrom-Json).analysis_index_allocations
            if ($indexAllocation.live_requested_bytes -ne 0) { throw "Index allocations remain live at process exit: $log" }
            $lines = @(Get-Content -LiteralPath $csv)
            if ($lines.Count -lt 3) { throw "Missing detector rows: $csv" }
            $rows = @(foreach ($line in ($lines | Select-Object -Skip 2)) {
                $fields = $line.Split(',')
                if ($fields.Length -lt 3) { throw "Invalid detector row: $line" }
                $counts = @($fields | Select-Object -Skip 3 | ForEach-Object { [long]$_ })
                $key = "$modelPath|$($fields[1])"
                $countVector = [string]::Join(',', $counts)
                if ($expectedCounts.ContainsKey($key)) {
                    if ($expectedCounts[$key] -ne $countVector) { throw "Detector count mismatch: $backend $modelPath stage $($fields[1])" }
                } else { $expectedCounts[$key] = $countVector }
                [ordered]@{ run = [int]$fields[0]; stage = $fields[1];
                    milliseconds = [double]::Parse($fields[2], [Globalization.CultureInfo]::InvariantCulture); counts = $counts }
            })
            if ([string]::Join(',', @($rows.run | Sort-Object -Unique)) -ne '0,1,2') { throw "Expected three complete detector runs: $csv" }
            $layout = $lines[0] + '|' + $lines[1] + '|' + [string]::Join('|', @($rows | ForEach-Object { "$($_.run),$($_.stage),$($_.counts.Count)" }))
            if ($expectedLayout.ContainsKey($modelPath)) {
                if ($expectedLayout[$modelPath] -ne $layout) { throw "Detector input/stage layout mismatch: $backend $modelPath" }
            } else { $expectedLayout[$modelPath] = $layout }
            $records += [ordered]@{ model = $modelPath; input_bytes = (Get-Item -LiteralPath $modelPath).Length;
                model_sha256 = $hash; backend = $backend; order = $order; header = $lines[0];
                csv = [IO.Path]::GetFileName($csv); csv_sha256 = (Get-FileHash -LiteralPath $csv).Hash.ToLowerInvariant();
                log = [IO.Path]::GetFileName($log); log_sha256 = (Get-FileHash -LiteralPath $log).Hash.ToLowerInvariant();
                index_allocations = $indexAllocation; rows = $rows }
        }
    }
}
$finalHashes = Get-DenseSetSourceHashes -RepositoryRoot $repositoryRoot -BuildDirectory $buildPath
foreach ($path in $sourceHashes.Keys) { if ($finalHashes[$path] -ne $sourceHashes[$path]) { throw "Source changed during campaign: $path" } }
foreach ($backend in $binaries.Keys) {
    if ((Get-FileHash -LiteralPath $binaries[$backend] -Algorithm SHA256).Hash.ToLowerInvariant() -ne $binaryHashes[$backend]) { throw "Binary changed during campaign: $backend" }
}
$processor = Get-CimInstance Win32_Processor | Select-Object -First 1
[ordered]@{
    base_commit = (& git -C $repositoryRoot rev-parse HEAD).Trim(); configuration = 'Release';
    started_utc = $started.ToString('o'); completed_utc = [datetime]::UtcNow.ToString('o');
    cpu = $processor.Name.Trim(); physical_cores = $processor.NumberOfCores; logical_processors = $processor.NumberOfLogicalProcessors;
    executables = $binaryHashes; sources = $sourceHashes; micro = $micro; detectors = $records;
    all_detector_count_vectors_equal = $true;
    method = 'Serial exclusive Release window. Micro has nine rotated four-backend samples, one warmup, fixed shuffled keys, insertion plus three hit passes. Both micro and pipeline use matched analysis-index TrackingAllocator counters on allocation events only, including all allocator rebinds; all other application allocations untracked. Pipeline uses unchanged current detector harness: two fresh three-run processes per backend per model, reversed backend order. Process-exit summary records aggregate index peak/traffic/calls and requires zero live bytes. Import/render excluded. Every computeComparisonStages call receives default empty analysisInputs; no application analysis cache is retained or reused between calls. First and later repetitions can differ through allocator/OS/input-data warmth. Full trailing count vectors/header/stage layouts validated. Pipeline default set uses append/spare strategy, stores keys once and preserves first-use IDs within checked 32-bit library domain; production needs custom wide fallback. Big-bucket and default find-first sets are micro-only. Counts do not establish complete finding/export/order equivalence.'
} | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $Output -Encoding utf8
