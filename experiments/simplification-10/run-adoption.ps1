param(
    [Parameter(Mandatory=$true)][string]$Model,
    [ValidateRange(3,20)][int]$Runs=7,
    [string]$Executable=(Join-Path $PSScriptRoot '../../build/vs2026-vcpkg/bin/Release/woby_intersection_adoption_benchmark.exe'),
    [string]$OutputDirectory=(Join-Path $PSScriptRoot '../../build/intersection-predicate-adoption')
)
$ErrorActionPreference='Stop'
$repoPath=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$modelPath=(Resolve-Path -LiteralPath $Model).Path
$executablePath=(Resolve-Path -LiteralPath $Executable).Path
$outputPath=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $outputPath) { throw "Choose a new output directory: $outputPath" }
New-Item -ItemType Directory -Path $outputPath | Out-Null
$sourceHashes=[ordered]@{}
foreach($relative in @('src/mesh_intersections.cpp','src/mesh_intersection_predicates.cpp',
    'src/mesh_intersection_predicates.h','CMakeLists.txt','experiments/simplification-10/main.cpp',
    'experiments/simplification-10/CMakeLists.txt','experiments/simplification-10/run-adoption.ps1',
    'experiments/simplification-10/predicate.cpp','experiments/simplification-10/probe.h',
    'experiments/simplification-10/rational-baseline.cpp',
    'build/vs2026-vcpkg/experiments/simplification-10/production_rational_baseline.cpp',
    'build/vs2026-vcpkg/vcpkg_installed/x64-windows/include/predicates.h')) {
    $sourceHashes[$relative]=(Get-FileHash -LiteralPath (Join-Path $repoPath $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$executableHash=(Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash.ToLowerInvariant()
$metadata=[ordered]@{
    base_commit=(& git -C $repoPath rev-parse HEAD)
    started_utc=[DateTime]::UtcNow.ToString('o')
    cpu=(Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    executable_sha256=$executableHash
    sources=$sourceHashes
    model=$modelPath
    model_bytes=(Get-Item -LiteralPath $modelPath).Length
    model_sha256=(Get-FileHash -LiteralPath $modelPath -Algorithm SHA256).Hash.ToLowerInvariant()
    runs_per_variant=$Runs
    scope='Normal allocator. Production library versus an uninstrumented rational baseline. Alternating order; import, geometry construction and complete result comparisons are outside the timer. Run without concurrent builds, tests or benchmarks.'
}
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outputPath 'metadata.json') -Encoding utf8NoBOM
foreach($case in @(@{label='synthetic';argument='--synthetic'},@{label='model';argument=('"'+$modelPath+'"')})) {
    $process=Start-Process -FilePath $executablePath -ArgumentList @($case.argument,$Runs) -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $outputPath ($case.label+'.json')) `
        -RedirectStandardError (Join-Path $outputPath ($case.label+'.log'))
    $process.WaitForExit()
    if($process.ExitCode -ne 0) { throw "Benchmark failed: $($case.label), exit $($process.ExitCode)" }
    $null=Get-Content -LiteralPath (Join-Path $outputPath ($case.label+'.json')) -Raw | ConvertFrom-Json -Depth 100
}
if((Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $executableHash) { throw 'Executable changed during measurement.' }
foreach($relative in $sourceHashes.Keys) {
    if((Get-FileHash -LiteralPath (Join-Path $repoPath $relative) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sourceHashes[$relative]) {
        throw "Source changed during measurement: $relative"
    }
}
$metadata['completed_utc']=[DateTime]::UtcNow.ToString('o')
$metadata['source_and_executable_hashes_unchanged']=$true
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outputPath 'metadata.json') -Encoding utf8NoBOM
Write-Output "Saved production adoption measurements to $outputPath"
