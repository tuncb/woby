# Pinned build tools for CI. Runtime drivers are supplied by the target machine.
param([Parameter(Mandatory)][string]$Destination)
$ErrorActionPreference = 'Stop'
$Destination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force $Destination | Out-Null
function Invoke-Checked([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Command failed ($LASTEXITCODE)" }
}
function Get-Source([string]$Repository, [string]$Revision, [string]$Name) {
    $path = Join-Path $Destination $Name
    if (-not (Test-Path "$path/.git")) {
        Invoke-Checked git @('init', $path)
        Invoke-Checked git @('-C', $path, 'remote', 'add', 'origin', "https://github.com/$Repository.git")
    }
    Invoke-Checked git @('-C', $path, 'fetch', '--depth=1', 'origin', $Revision)
    Invoke-Checked git @('-C', $path, 'checkout', '--detach', $Revision)
    # CMake macro arguments reparse backslashes as escapes (for example D:\a).
    return $path.Replace('\', '/')
}
function Build-Installed([string]$Source, [string]$Name, [string[]]$Options) {
    $build = Join-Path $Destination "$Name-build"
    Invoke-Checked cmake (@('-S', $Source, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_LIBDIR=lib', "-DCMAKE_INSTALL_PREFIX=$sdk", "-DCMAKE_PREFIX_PATH=$sdk") + $Options)
    Invoke-Checked cmake @('--build', $build, '--parallel', '2')
    Invoke-Checked cmake @('--install', $build)
}
if ($IsWindows) {
    $asset = 'windows-x86_64.zip'
    $digest = '1339d7b3050ae680c11152b8ccff977173591ce8e9c1d2a5cff2eb904d52e50d'
} elseif ($IsMacOS) {
    $asset = 'macos-aarch64.tar.gz'
    $digest = '6c8bf066a4254e8d053f864a52ea18388702d409ed9f20b3d6da66e8d772c906'
} else {
    $asset = 'linux-x86_64-glibc-2.27.tar.gz'
    $digest = '373b57f6bc9e3dc3f75a95be8de17acc8fd2468f1e1d1c1f71dece86e7c7468a'
}
$archive = Join-Path $Destination "slang-$asset"
Invoke-WebRequest "https://github.com/shader-slang/slang/releases/download/v2026.18.3/slang-2026.18.3-$asset" -OutFile $archive
if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $digest) { throw 'Slang archive checksum mismatch' }
$slang = Join-Path $Destination 'slang'
New-Item -ItemType Directory -Force $slang | Out-Null
if ($IsWindows) { Expand-Archive -LiteralPath $archive -DestinationPath $slang -Force }
else { Invoke-Checked tar @('-xzf', $archive, '-C', $slang) }
"$slang/bin" >> $env:GITHUB_PATH
"SLANG_ROOT=$slang" >> $env:GITHUB_ENV
if ($IsMacOS) {
    # Metal 4 requires Xcode 26 and its Metal toolchain.
    Invoke-Checked xcrun @('-sdk', 'macosx', 'metal', '--version')
    exit 0
}
$sdk = (Join-Path $Destination 'vulkan').Replace('\', '/')
$headers = Get-Source 'KhronosGroup/Vulkan-Headers' 'e3b1eec08173d6b825cd3ac88c885a63b621504a' 'vulkan-headers'
Build-Installed $headers 'vulkan-headers' @('-DVULKAN_HEADERS_ENABLE_TESTS=OFF')
$loader = Get-Source 'KhronosGroup/Vulkan-Loader' '5f157b62e333c63260d05d81bf66faa216ab0fb8' 'vulkan-loader'
Build-Installed $loader 'vulkan-loader' @('-DBUILD_TESTS=OFF', '-DBUILD_WSI_WAYLAND_SUPPORT=OFF')
$spirvHeaders = Get-Source 'KhronosGroup/SPIRV-Headers' '29981f65241605e08b0ede4cfeb999fe3b723c6a' 'spirv-headers'
$spirvTools = Get-Source 'KhronosGroup/SPIRV-Tools' '9a49b0883b9b635689a85b5647dbfcb223268151' 'spirv-tools'
Build-Installed $spirvTools 'spirv-tools' @("-DSPIRV-Headers_SOURCE_DIR=$spirvHeaders", '-DSPIRV_SKIP_TESTS=ON', '-DSPIRV_SKIP_EXECUTABLES=OFF', '-DSPIRV_TOOLS_BUILD_STATIC=ON')
"VULKAN_SDK=$sdk" >> $env:GITHUB_ENV
"$sdk/bin" >> $env:GITHUB_PATH
