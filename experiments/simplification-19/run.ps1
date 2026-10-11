param([string]$BinaryDirectory = (Join-Path $PSScriptRoot '../../build/vs2026-vcpkg/bin/Release'),
      [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../doc/benchmarks/simplification-19'),
      [string]$Output = (Join-Path $PSScriptRoot '../../doc/simplification-19-results.json'),
      [string[]]$Models = @('D:/temp/obj_tests/uploads_files_2720101_BusGameMap.obj', 'D:/temp/obj_tests/powerplant.obj'))
$ErrorActionPreference = 'Stop'
if ((Test-Path -LiteralPath $Output) -or (Test-Path -LiteralPath $OutputDirectory)) { throw 'Output already exists.' }
$binaryPath = (Resolve-Path -LiteralPath $BinaryDirectory).Path
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputPath | Out-Null
$microExe = Join-Path $binaryPath 'woby_analysis_index_probe.exe'
$micro = (& $microExe | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0) { throw 'Analysis index microbenchmark failed.' }
$binaries = [ordered]@{ custom = 'woby_benchmarks.exe'; boost = 'woby_benchmarks_boost.exe'; ankerl = 'woby_benchmarks_ankerl.exe' }
$binaryHashes = [ordered]@{ micro = (Get-FileHash -LiteralPath $microExe).Hash }
foreach ($backend in $binaries.Keys) { $binaryHashes[$backend] = (Get-FileHash -LiteralPath (Join-Path $binaryPath $binaries[$backend])).Hash }
$records = @()
$expectedCounts = @{}
$expectedLayout = @{}
foreach ($model in $Models) {
    $modelPath = (Resolve-Path -LiteralPath $model).Path
    $hash = (Get-FileHash -LiteralPath $modelPath).Hash
    $label = [IO.Path]::GetFileNameWithoutExtension($modelPath)
    foreach ($order in 0..1) {
        $backends = if ($order -eq 0) { @('custom','boost','ankerl') } else { @('ankerl','boost','custom') }
        foreach ($backend in $backends) {
            $csv = Join-Path $outputPath "$label-$order-$backend.csv"
            $log = Join-Path $outputPath "$label-$order-$backend.log"
            $process = Start-Process -FilePath (Join-Path $binaryPath $binaries[$backend]) `
                -ArgumentList @('detectors', ('"' + $modelPath + '"'), '3') -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput $csv -RedirectStandardError $log
            $process.WaitForExit()
            if ($process.ExitCode -ne 0) { throw "Detector probe failed: $backend $modelPath. See $log." }
            $lines = Get-Content -LiteralPath $csv
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
            $layout = $lines[0] + '|' + [string]::Join('|', @($rows | ForEach-Object { "$($_.run),$($_.stage),$($_.counts.Count)" }))
            if ($expectedLayout.ContainsKey($modelPath)) {
                if ($expectedLayout[$modelPath] -ne $layout) { throw "Detector input/stage layout mismatch: $backend $modelPath" }
            } else { $expectedLayout[$modelPath] = $layout }
            $records += [ordered]@{ model = $modelPath; input_bytes = (Get-Item -LiteralPath $modelPath).Length;
                model_sha256 = $hash; backend = $backend; order = $order; header = $lines[0];
                csv = [IO.Path]::GetFileName($csv); csv_sha256 = (Get-FileHash -LiteralPath $csv).Hash; rows = $rows }
        }
    }
}
$hashes = [ordered]@{}
foreach ($name in @('main.cpp','CMakeLists.txt','run.ps1','../../src/analysis_index.h','../../tests/performance_benchmarks.cpp','../../tests/benchmark_analysis_index.h',
    '../../build/vs2026-vcpkg/hash-probe/mesh_comparison.cpp','../../build/vs2026-vcpkg/hash-probe/mesh_duplicates.cpp','../../build/vs2026-vcpkg/hash-probe/mesh_topology.cpp')) {
    $path = Join-Path $PSScriptRoot $name
    if (Test-Path -LiteralPath $path) { $hashes[$name] = (Get-FileHash -LiteralPath $path).Hash }
}
[ordered]@{ base_commit = (& git -C $PSScriptRoot rev-parse HEAD); date = (Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz');
    cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name; executables = $binaryHashes; sources = $hashes;
    micro = $micro; detectors = $records; all_detector_count_vectors_equal = $true;
    method = 'Serial Release runs. Microbenchmark warms all paths and rotates/alternates order. Real-model stage batches use three runs in each fresh process, then reverse backend order; load/render excluded. Every trailing detector count is retained and validated across all runs/backends. No concurrent builds or tests.'
} | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $Output -Encoding utf8
