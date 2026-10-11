param([string]$BinaryDirectory = (Join-Path $PSScriptRoot '../../build/vs2026-vcpkg/bin/Release'),
      [string]$Output = (Join-Path $PSScriptRoot '../../doc/simplification-14-results.json'),
      [string[]]$Models = @('D:/temp/obj_tests/uploads_files_2720101_BusGameMap.obj',
          'D:/temp/obj_tests/powerplant.obj', 'D:/temp/obj_tests/san-miguel.obj',
          'D:/temp/obj_tests/bennu_OLA_v21_PTM_very-high.obj',
          'D:/temp/obj_tests/20190810_BearTrap_Ground_Model.obj'))
$ErrorActionPreference = 'Stop'
$exe = Join-Path (Resolve-Path -LiteralPath $BinaryDirectory).Path 'woby_simplification_14.exe'
if (Test-Path -LiteralPath $Output) { throw "Output already exists: $Output" }
$records = @()
foreach ($model in $Models) {
    $modelPath = (Resolve-Path -LiteralPath $model).Path
    # Outside timing; this also warms input. These are repeated-load tests.
    $hash = (Get-FileHash -LiteralPath $modelPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $record = (& $exe $modelPath | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0 -or -not $record.ok) { throw "OBJ corner probe failed: $modelPath" }
    $record | Add-Member -NotePropertyName sha256 -NotePropertyValue $hash
    $record | Add-Member -NotePropertyName input_bytes -NotePropertyValue (Get-Item -LiteralPath $modelPath).Length
    $records += $record
}
[ordered]@{ ok = $true; records = $records; method = 'Serial fresh process per model; five rotated in-process packing runs; validation untimed; SHA256 pre-read warms input.' } |
    ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $Output -Encoding utf8
