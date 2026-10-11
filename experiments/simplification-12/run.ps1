param([string]$BinaryDirectory = (Join-Path $PSScriptRoot '../../build/vs2026-vcpkg/bin/Release'),
      [string]$Output = (Join-Path $PSScriptRoot '../../doc/simplification-12-results.json'))
$ErrorActionPreference = 'Stop'
$exe = Join-Path (Resolve-Path -LiteralPath $BinaryDirectory).Path 'woby_simplification_12.exe'
if (Test-Path -LiteralPath $Output) { throw "Output already exists: $Output" }
$record = (& $exe | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0 -or -not $record.ok) { throw 'Graphics parameter probe failed' }
$record | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $Output -Encoding utf8
