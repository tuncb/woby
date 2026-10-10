param([string]$BuildDirectory = (Join-Path $PSScriptRoot '../../build/vs2026-vcpkg'))
$ErrorActionPreference = 'Stop'
$buildPath = (Resolve-Path -LiteralPath $BuildDirectory).Path
$cache = Get-Content -LiteralPath (Join-Path $buildPath 'CMakeCache.txt')
$slang = ($cache | Where-Object { $_ -like 'WOBY_SLANGC:FILEPATH=*' }) -replace '^WOBY_SLANGC:FILEPATH=', ''
$validator = ($cache | Where-Object { $_ -like 'WOBY_SPIRV_VAL:FILEPATH=*' }) -replace '^WOBY_SPIRV_VAL:FILEPATH=', ''
$outputPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../build/face-display'))
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$source = Join-Path $PSScriptRoot 'quality_faces.slang'
$arguments = @('-entry','vs_quality_faces','-stage','vertex','-warnings-as-errors','all',
    '-fvk-use-c-layout','-matrix-layout-row-major',
    '-I',(Join-Path $buildPath '_deps/woby_ngapi-src/include'),
    '-I',(Join-Path $buildPath '_deps/woby_ngapi-src/utility/include'))
& $slang $source @arguments -target spirv -profile spirv_1_5 -emit-spirv-directly -fvk-use-entrypoint-name -DWOBY_VULKAN -capability spvDescriptorHeapEXT -o (Join-Path $outputPath 'quality.spv')
if ($LASTEXITCODE -ne 0) { throw 'SPIR-V compilation failed' }
& $validator --target-env vulkan1.4 --scalar-block-layout (Join-Path $outputPath 'quality.spv')
if ($LASTEXITCODE -ne 0) { throw 'SPIR-V validation failed' }
& $slang $source @arguments -target metal -DNOGRAPHICSAPI_METAL -o (Join-Path $outputPath 'quality.metal')
if ($LASTEXITCODE -ne 0) { throw 'Metal source generation failed' }
Write-Output 'SPIR-V compiled and validated; Metal source generated. No GPU execution or Apple metallib build was performed.'
