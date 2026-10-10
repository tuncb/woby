param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../build/source-position-storage'),
    [ValidateRange(1, 100)][int]$Runs = 3
)

$ErrorActionPreference = 'Stop'
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'Run this script from an x64 MSVC developer PowerShell.'
}
$probeDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $probeDirectory | Out-Null
$source = Join-Path $PSScriptRoot 'source_position_storage.cpp'
$executable = Join-Path $probeDirectory 'source-position-storage.exe'
$object = Join-Path $probeDirectory 'source-position-storage.obj'
& cl.exe /nologo /O2 /MD /std:c++20 /EHsc /W4 /WX /fp:precise "/Fe:$executable" "/Fo:$object" $source
if ($LASTEXITCODE -ne 0) { throw 'Probe compilation failed.' }

$rows = @()
foreach ($run in 1..$Runs) {
    $output = & $executable $run
    if ($LASTEXITCODE -ne 0) { throw "Probe run $run failed." }
    $sample = @($output | ConvertFrom-Csv)
    if ($sample.Count -ne 28) { throw "Probe run $run has an incomplete result." }
    $rows += $sample
}
$rows | Export-Csv -NoTypeInformation -LiteralPath (Join-Path $probeDirectory 'results.csv')
$metadata = [ordered]@{
    date = (Get-Date -Format 'yyyy-MM-dd')
    repository_commit = (& git -C $PSScriptRoot rev-parse HEAD)
    source_sha256 = (Get-FileHash -LiteralPath $source).Hash
    executable_sha256 = (Get-FileHash -LiteralPath $executable).Hash
    compiler_path = (Get-Command cl.exe).Source
    compiler_file_version = (Get-Item (Get-Command cl.exe).Source).VersionInfo.FileVersion
    compiler_flags = '/O2 /MD /std:c++20 /EHsc /W4 /WX /fp:precise'
    cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    os = (Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version)
    physical_memory_bytes = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
    random_seed = 1729
    runs = $Runs
    samples_per_run = 9
    warmup_pairs = 1
    memory_values = 'Logical coordinate and mapping payloads; not process memory measurements'
}
$metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $probeDirectory 'metadata.json')
Write-Output "Results: $probeDirectory"
