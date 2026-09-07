function ConvertFrom-LddOutput {
    param([string[]]$Lines)

    foreach ($line in $Lines) {
        # Require the load address. With multiple inputs, ldd also emits
        # absolute executable names followed by a colon as section headers.
        if ($line -match '^\s*\S+\s+=>\s+(/.+?)\s+\(0x[0-9a-fA-F]+\)\s*$') {
            $matches[1]
        } elseif ($line -match '^\s*(/.+?)\s+\(0x[0-9a-fA-F]+\)\s*$') {
            $matches[1]
        }
    }
}
