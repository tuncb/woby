param([string]$BinaryDirectory = (Join-Path $PSScriptRoot '../../build/vs2026-vcpkg/bin/Release'),
      [string]$InputFile = (Join-Path $PSScriptRoot '../../doc/simplification-14-results.json'),
      [string]$Output = (Join-Path $PSScriptRoot '../../doc/simplification-14-capacity-results.json'))
$ErrorActionPreference = 'Stop'
$exe = Join-Path (Resolve-Path -LiteralPath $BinaryDirectory).Path 'woby_simplification_14_capacity.exe'
$source = (Resolve-Path -LiteralPath $InputFile).Path
if (Test-Path -LiteralPath $Output) { throw "Output already exists: $Output" }
$record = (& $exe $source | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0 -or -not $record.ok) { throw 'Capacity probe failed' }
$record | Add-Member -NotePropertyName input_sha256 -NotePropertyValue (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$record | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $Output -Encoding utf8
