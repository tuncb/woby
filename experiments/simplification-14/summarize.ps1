param([string]$InputFile = (Join-Path $PSScriptRoot '../../doc/simplification-14-results.json'),
      [string]$Output = (Join-Path $PSScriptRoot '../../doc/simplification-14-derived-payloads.json'))
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Output) { throw "Output already exists: $Output" }
$inputRecord = Get-Content -LiteralPath $InputFile -Raw | ConvertFrom-Json
if (-not $inputRecord.ok) { throw 'Input result is not successful' }
$rows = foreach ($record in $inputRecord.records) {
    $s = [long]$record.source_positions
    $n = [long]$record.normal_records
    $u = [long]$record.uv_records
    $c = [long]$record.corners
    $r = [long]$record.render_vertices
    $attributes = 12 * $s + 12 * $n + 8 * $u
    $equivalentAttributes = 12 * $s + 8 * $u
    if ($record.complete_authored_normals) { $equivalentAttributes += 12 * $n }
    else { $equivalentAttributes += 12 * $r }
    $indexed = $equivalentAttributes + 12 * $r + 4 * $c
    $generatedImplicit = $null
    if (-not $record.complete_authored_normals) { $generatedImplicit = 12 * $s + 8 * $u + 20 * $r + 4 * $c }
    $noUvVertex = $null
    $noUvGeneratedTuple = $null
    if ($u -eq 0) {
        $noUvVertex = 24 * $r + 4 * $c
        if (-not $record.complete_authored_normals) { $noUvGeneratedTuple = 12 * $s + 16 * $r + 4 * $c }
    }
    [ordered]@{
        name = [IO.Path]::GetFileName($record.path)
        corners_per_render_vertex = $c / [double]$r
        current_gpu_bytes = $record.current_gpu_surface_bytes
        equivalent_corner_representatives_gpu_bytes = $record.equivalent_representative_direct_gpu_bytes
        equivalent_indexed_tuple_gpu_bytes = $indexed
        indexed_tuple_saved_bytes = $record.current_gpu_surface_bytes - $indexed
        indexed_tuple_saved_percent = 100 * ($record.current_gpu_surface_bytes - $indexed) / [double]$record.current_gpu_surface_bytes
        generated_implicit_normal_index_gpu_bytes = $generatedImplicit
        no_uv_24_byte_vertex_gpu_bytes = $noUvVertex
        no_uv_generated_4_byte_position_tuple_gpu_bytes = $noUvGeneratedTuple
        declared_raw_attributes_bytes = $attributes
        equivalent_attributes_bytes = $equivalentAttributes
    }
}
[ordered]@{
    scope = 'Exact payload calculations from raw counts; layouts are unimplemented and untimed. Generated-normal equivalence replaces declared normals with one 12-byte record per render ID. Source data, marker/node arrays, CPU retention and buffer allocation overhead are excluded.'
    indexed_tuple_formula = '12S+12N+8U+12R+4C; generated normals substitute R for N'
    implicit_generated_normal_formula = '12S+8U+20R+4C'
    no_uv_vertex_formula = '24R+4C'
    no_uv_generated_position_tuple_formula = '12S+16R+4C'
    rows = @($rows)
} | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Output -Encoding utf8
