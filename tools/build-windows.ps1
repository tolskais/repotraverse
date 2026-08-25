[CmdletBinding()]
param(
    [ValidateSet("Generic", "Native")]
    [string]$Mode = "Generic",

    [string]$LlvmRoot = $env:REPOTRAVERSE_LLVM_ROOT,

    [ValidateRange(1, 64)]
    [int]$Jobs = 2
)

$ErrorActionPreference = "Stop"
$sourceRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($LlvmRoot)) {
    throw "Provide -LlvmRoot or set REPOTRAVERSE_LLVM_ROOT."
}

$clangCompiler = Join-Path $LlvmRoot "bin/clang-cl.exe"
if (-not (Test-Path -LiteralPath $clangCompiler -PathType Leaf)) {
    throw "LLVM SDK file is missing: $clangCompiler"
}
$env:REPOTRAVERSE_LLVM_ROOT = (Resolve-Path -LiteralPath $LlvmRoot).Path

$preset = switch ($Mode) {
    "Generic" { "windows-x64-generic" }
    "Native" { "windows-x64-native" }
}

Push-Location $sourceRoot
try {
    & cmake --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

    & cmake --build --preset "$($preset)-release" --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }

    & ctest --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "Tests failed." }
}
finally {
    Pop-Location
}
