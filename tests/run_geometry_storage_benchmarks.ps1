param(
    [Parameter(Mandatory)][string]$BeforeExecutable,
    [Parameter(Mandatory)][string]$AfterExecutable,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [int]$Runs = 3
)
$ErrorActionPreference = 'Stop'
$before = (Resolve-Path -LiteralPath $BeforeExecutable).Path
$after = (Resolve-Path -LiteralPath $AfterExecutable).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$records = @()
$expectedCounts = @{}
for ($run = 1; $run -le $Runs; $run++) {
    $order = if ($run % 2) { @('before', 'after') } else { @('after', 'before') }
    foreach ($variant in $order) {
        $exe = if ($variant -eq 'before') { $before } else { $after }
        $stdout = Join-Path $output "$variant-$run.txt"
        $stderr = Join-Path $output "$variant-$run.stderr.txt"
        $process = Start-Process -FilePath $exe -ArgumentList '--test-case="geometry storage benchmark" --no-skip=true' `
            -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $peakPrivate = 0L
        $peakWorking = 0L
        while (-not $process.HasExited) {
            $process.Refresh()
            $peakPrivate = [Math]::Max($peakPrivate, $process.PrivateMemorySize64)
            $peakWorking = [Math]::Max($peakWorking, $process.PeakWorkingSet64)
            Start-Sleep -Milliseconds 10
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "$variant benchmark failed: $($process.ExitCode)" }
        $rows = Get-Content -LiteralPath $stdout | Where-Object { $_ -match '^(snapshot_one_part|undo_prepare|comparison_source|annotation_prepare),' }
        if ($rows.Count -ne 8) { throw "$variant benchmark did not report all workloads" }
        foreach ($row in $rows) {
            $fields = $row.Split(',')
            $key = "$($fields[0])/$($fields[1])"
            if ($expectedCounts.ContainsKey($key) -and $expectedCounts[$key] -ne $fields[6]) {
                throw "$variant changed the result count for $key"
            }
            $expectedCounts[$key] = $fields[6]
        }
        $records += [pscustomobject]@{
            variant = $variant; run = $run
            sampled_peak_private_bytes = $peakPrivate
            peak_working_set_bytes = $peakWorking
            executable_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash
        }
    }
}
$records | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $output 'process-memory.json')
