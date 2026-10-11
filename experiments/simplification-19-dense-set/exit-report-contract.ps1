param([Parameter(Mandatory)][string]$CustomExe, [Parameter(Mandatory)][string]$DenseExe)
$ErrorActionPreference = 'Stop'
$fixtureBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixtureRoot = Join-Path $fixtureBase ('woby-dense-set-exit-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
try {
    $model = Join-Path $fixtureRoot 'fixture.obj'
    @('v 0 0 0', 'v 1 0 0', 'v 0 1 0', 'v 0 0 1', 'f 1 2 3', 'f 1 2 3', 'f 1 2 4') |
        Set-Content -LiteralPath $model -Encoding ascii
    $summaries = [ordered]@{}
    $expected = $null
    foreach ($backend in @('custom','dense_set')) {
        $executable = if ($backend -eq 'custom') { $CustomExe } else { $DenseExe }
        $csv = Join-Path $fixtureRoot "$backend.csv"
        $log = Join-Path $fixtureRoot "$backend.log"
        $process = Start-Process -FilePath $executable -ArgumentList @('detectors', ('"' + $model + '"'), '1') `
            -WindowStyle Hidden -PassThru -RedirectStandardOutput $csv -RedirectStandardError $log
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "Exit-report fixture failed: $backend" }
        $reports = @(Get-Content -LiteralPath $log | Where-Object { $_ -match '^\{"analysis_index_allocations":' })
        if ($reports.Count -ne 1) { throw "Expected one exit report: $backend" }
        $summary = ($reports[0] | ConvertFrom-Json).analysis_index_allocations
        if ($summary.live_requested_bytes -ne 0 -or $summary.peak_requested_bytes -le 0 `
            -or $summary.total_requested_bytes -lt $summary.peak_requested_bytes -or $summary.allocation_calls -le 0) {
            throw "Invalid exit allocation ledger: $backend"
        }
        $summaries[$backend] = $summary
        $lines = @(Get-Content -LiteralPath $csv)
        if ($lines.Count -ne 5) { throw "Expected three stage/total rows: $backend" }
        $contract = $lines[0] + '|' + $lines[1] + '|' + [string]::Join('|', @($lines | Select-Object -Skip 2 | ForEach-Object {
            $fields = $_.Split(','); "$($fields[0]),$($fields[1])," + [string]::Join(',', @($fields | Select-Object -Skip 3))
        }))
        if ($null -ne $expected -and $contract -ne $expected) { throw 'Fixture detector counts/layout differ.' }
        $expected = $contract
    }
    [ordered]@{ ok=$true; detector_processes=2; fixture_contract=$expected; allocations=$summaries } | ConvertTo-Json -Depth 10 -Compress
} finally {
    $resolvedFixture = [IO.Path]::GetFullPath($fixtureRoot)
    if ([IO.Path]::GetDirectoryName($resolvedFixture).TrimEnd('\','/') -ne $fixtureBase.TrimEnd('\','/') `
        -or -not [IO.Path]::GetFileName($resolvedFixture).StartsWith('woby-dense-set-exit-')) {
        throw 'Unsafe temporary-fixture cleanup path.'
    }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
