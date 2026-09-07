$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/linux-runtime-dependencies.ps1"

$lines = @(
    '/home/runner/work/woby/build/bin/woby:',
    '    linux-vdso.so.1 (0x00007fff12345000)',
    '    libSDL3.so.0 => /home/runner/work/woby/lib/libSDL3.so.0 (0x00007fa012340000)',
    '    libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007fa012350000)',
    '    /lib64/ld-linux-x86-64.so.2 (0x00007fa012360000)',
    '/home/runner/work/woby/build/bin/woby-update-helper:',
    '    libcrypto.so.3 => /home/runner/work/woby/lib/libcrypto.so.3 (0x00007fa012370000)',
    '    libmissing.so => not found',
    '    libspaces.so => /home/runner/work/space directory/libspaces.so (0xABCDEF)',
    '    statically linked'
)
$expected = @(
    '/home/runner/work/woby/lib/libSDL3.so.0',
    '/lib/x86_64-linux-gnu/libc.so.6',
    '/lib64/ld-linux-x86-64.so.2',
    '/home/runner/work/woby/lib/libcrypto.so.3',
    '/home/runner/work/space directory/libspaces.so'
)
$actual = @(ConvertFrom-LddOutput $lines)
if (($actual -join "`n") -cne ($expected -join "`n")) {
    throw "Incorrect runtime dependencies: $($actual -join ', ')"
}
if (@(ConvertFrom-LddOutput @('/tmp/woby:', '/tmp/woby-update-helper:')).Count -ne 0) {
    throw 'Executable section headers must not become runtime dependencies.'
}
Write-Output 'Linux runtime dependency parser tests passed.'
