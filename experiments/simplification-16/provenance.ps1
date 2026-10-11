function Get-SimplificationProvenance {
    param(
        [string]$RepositoryRoot,
        [string[]]$Paths,
        [datetime]$CompletedAt = [datetime]::UtcNow
    )
    $hashes = [ordered]@{}
    foreach ($relative in $Paths) {
        $hashes[$relative] = (Get-FileHash -LiteralPath (Join-Path $RepositoryRoot $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $processor = Get-CimInstance Win32_Processor | Select-Object -First 1
    $baseCommit = (& git -C $RepositoryRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read the production base commit' }
    [ordered]@{
        base_commit = $baseCommit
        measurement_completed_utc = $CompletedAt.ToUniversalTime().ToString('o')
        cpu = $processor.Name.Trim()
        physical_cores = $processor.NumberOfCores
        logical_processors = $processor.NumberOfLogicalProcessors
        file_sha256 = $hashes
    }
}
