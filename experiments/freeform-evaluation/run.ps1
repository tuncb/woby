param(
    [string]$Executable = "$PSScriptRoot/../../build/vs2026-vcpkg/bin/Release/woby_freeform_evaluation_probe.exe",
    [string[]]$Models = @('--synthetic', 'D:/temp/obj_tests/parametric/tubes_as_nurbs.obj', 'D:/temp/obj_tests/parametric/SA_Row3_x01000.obj'),
    [string]$Output = "$PSScriptRoot/../../build/freeform-evaluation/results.json"
)
$ErrorActionPreference = 'Stop'
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$sourceRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$results = @()
foreach ($model in $Models) {
    $inputPath = if ($model -eq '--synthetic') { $model } else { (Resolve-Path -LiteralPath $model).Path }
    $lines = & $executablePath $inputPath
    if ($LASTEXITCODE -ne 0) { throw "Probe failed for $inputPath" }
    $measurement = ($lines -join "`n") | ConvertFrom-Json
    $inputHash = if ($model -eq '--synthetic') { $null } else { (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash }
    $results += @{ measurement = $measurement; input_sha256 = $inputHash }
}
$record = @{
    recorded_at = (Get-Date).ToString('o')
    base_commit = (& git -C $sourceRoot rev-parse HEAD)
    cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name)
    build = 'Visual Studio 2026 x64 Release, vs2026-vcpkg'
    executable_sha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash
    probe_sha256 = (Get-FileHash -LiteralPath "$PSScriptRoot/main.cpp" -Algorithm SHA256).Hash
    scope = 'CPU sample and normal loop: parameter-based wrapper versus production sample reuse. No GPU dispatch, upload, or full viewer timing. Five warm evaluation pairs with alternating order.'
    results = $results
}
$outputPath = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path (Split-Path -Parent $outputPath) -Force | Out-Null
$record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $outputPath -Encoding utf8
Write-Output $outputPath
