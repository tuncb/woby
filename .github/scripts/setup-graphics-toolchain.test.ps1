# Run the real setup script with offline dependency fixtures. CMake still runs,
# including a macro expansion like the one in SPIRV-Tools that rejected '\a'.
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) {
    Write-Output 'Windows graphics toolchain path regression skipped on this platform.'
    exit 0
}
$setupScript = Join-Path $PSScriptRoot 'setup-graphics-toolchain.ps1'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) "woby graphics $([guid]::NewGuid().ToString('N'))"
$previousGithubPath = $env:GITHUB_PATH
$previousGithubEnv = $env:GITHUB_ENV
$previousPath = $env:PATH
try {
    New-Item -ItemType Directory -Path $testRoot | Out-Null
    # Hosted Windows runners expose several CMake installations. Add another
    # executable candidate so this also exercises command selection locally.
    $extraCmake = Join-Path $testRoot 'second cmake/bin'
    New-Item -ItemType Directory -Path $extraCmake -Force | Out-Null
    New-Item -ItemType File -Path "$extraCmake/cmake.exe" | Out-Null
    $env:PATH += ";$extraCmake"
    $cmakeCommands = @(Get-Command cmake -CommandType Application)
    if ($cmakeCommands.Count -lt 2) { throw 'Expected multiple CMake candidates.' }
    $cmakeExecutable = $cmakeCommands[0].Source
    $env:GITHUB_PATH = Join-Path $testRoot 'github-path'
    $env:GITHUB_ENV = Join-Path $testRoot 'github-env'
    & {
        function Invoke-WebRequest { param($Uri, $OutFile) }
        function Get-FileHash {
            param($Path, $Algorithm)
            return @{ Hash = '1339d7b3050ae680c11152b8ccff977173591ce8e9c1d2a5cff2eb904d52e50d' }
        }
        function Expand-Archive { param($LiteralPath, $DestinationPath, [switch]$Force) }
        function git {
            if ($args[0] -eq 'init') {
                $source = $args[1]
                New-Item -ItemType Directory -Force -Path "$source/.git" | Out-Null
                Set-Content -LiteralPath "$source/fixture.txt" -Value 'dependency fixture'
                Set-Content -LiteralPath "$source/CMakeLists.txt" -Value @'
cmake_minimum_required(VERSION 3.24)
project(GraphicsToolchainFixture NONE)
# Macro substitution reparses its arguments, matching SPIRV-Tools' failure.
macro(check_header_path source)
    if(NOT EXISTS "${source}/fixture.txt")
        message(FATAL_ERROR "Missing dependency fixture: ${source}")
    endif()
endmacro()
if(DEFINED SPIRV-Headers_SOURCE_DIR)
    check_header_path("${SPIRV-Headers_SOURCE_DIR}")
    file(WRITE "${CMAKE_BINARY_DIR}/headers-checked.txt" "checked")
endif()
'@
            }
            $global:LASTEXITCODE = 0
        }
        function cmake {
            if ($args[0] -eq '-S') {
                # No compiler or network is needed; exercise real CMake argument
                # parsing and dependency lookup, including paths with spaces.
                & $cmakeExecutable @args --no-warn-unused-cli
            } else {
                $global:LASTEXITCODE = 0
            }
        }
        & $setupScript -Destination (Join-Path $testRoot 'a tools')
    }
    if (-not (Test-Path -LiteralPath "$testRoot/a tools/spirv-tools-build/headers-checked.txt")) {
        throw 'The SPIRV-Tools CMake path check did not run.'
    }
    Write-Output 'Windows graphics toolchain path regression passed.'
} finally {
    $env:GITHUB_PATH = $previousGithubPath
    $env:GITHUB_ENV = $previousGithubEnv
    $env:PATH = $previousPath
    $resolvedRoot = [IO.Path]::GetFullPath($testRoot)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolvedRoot.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove fixture outside the temporary directory: $resolvedRoot"
    }
    Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
}
