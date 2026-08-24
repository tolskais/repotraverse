[CmdletBinding()]
param(
    [ValidateSet("Generic", "Native")]
    [string]$Mode = "Generic"
)

$ErrorActionPreference = "Stop"
$sourceRoot = Split-Path -Parent $PSScriptRoot
$preset = if ($Mode -eq "Native") { "windows-x64-native" } else { "windows-x64-generic" }
$stage = Join-Path $sourceRoot "out\package\$preset"
$archive = Join-Path $sourceRoot "out\package\repotraverse-$preset.zip"

if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage | Out-Null
cmake --install (Join-Path $sourceRoot "out\build\$preset") --prefix $stage
if ($LASTEXITCODE -ne 0) { throw "Install staging failed." }

$executables = Get-ChildItem -LiteralPath (Join-Path $stage "bin") -Filter "*.exe"

$extractorVersion = & (Join-Path $stage "bin\clang-extractor.exe") --version
if ($extractorVersion -notmatch "LLVM/Clang ([0-9.]+)/") {
    throw "Could not determine the LLVM version from clang-extractor --version."
}
$llvmVersion = $Matches[1]
$resourceVersions = Get-ChildItem -LiteralPath (Join-Path $stage "lib\clang") `
    -Directory
if ($resourceVersions.Count -ne 1 -or
    -not (Test-Path -LiteralPath (Join-Path $resourceVersions[0].FullName "include"))) {
    throw "The staged package does not contain one Clang resource directory."
}
$smokeRoot = Join-Path $stage "패키지 검사"
New-Item -ItemType Directory -Path $smokeRoot | Out-Null
$smokeSource = Join-Path $smokeRoot "한글 소스.cpp"
Set-Content -LiteralPath $smokeSource -Encoding utf8 `
    -Value "int packaged_extractor_smoke() { return 0; }"
$resourceOption = "-resource-dir=$($resourceVersions[0].FullName)"
& (Join-Path $stage "bin\clang-extractor.exe") $smokeSource -- `
    $resourceOption -std=c++17 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Packaged extractor smoke test failed." }
Remove-Item -LiteralPath $smokeRoot -Recurse -Force
$sbom = [ordered]@{
    spdxVersion = "SPDX-2.3"
    dataLicense = "CC0-1.0"
    SPDXID = "SPDXRef-DOCUMENT"
    name = "repotraverse-$preset"
    documentNamespace = "https://github.com/tolskais/repotraverse/spdx/$preset/$([guid]::NewGuid())"
    creationInfo = @{ created = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ"); creators = @("Tool: package-windows.ps1") }
    packages = @(
        @{ name = "repotraverse"; SPDXID = "SPDXRef-Package-Repotraverse"; versionInfo = "1.0.0"; downloadLocation = "https://github.com/tolskais/repotraverse"; filesAnalyzed = $false; licenseConcluded = "BSD-2-Clause"; licenseDeclared = "BSD-2-Clause"; comment = $extractorVersion },
        @{ name = "llvm-project"; SPDXID = "SPDXRef-Package-LLVM"; versionInfo = $llvmVersion; downloadLocation = "NOASSERTION"; filesAnalyzed = $false; licenseConcluded = "Apache-2.0 WITH LLVM-exception" },
        @{ name = "nlohmann-json"; SPDXID = "SPDXRef-Package-Json"; versionInfo = "3.12.0"; downloadLocation = "NOASSERTION"; filesAnalyzed = $false; licenseConcluded = "MIT" },
        @{ name = "sqlite"; SPDXID = "SPDXRef-Package-SQLite"; versionInfo = "3.53.4"; downloadLocation = "NOASSERTION"; filesAnalyzed = $false; licenseConcluded = "LicenseRef-Public-Domain" },
        @{ name = "xxhash"; SPDXID = "SPDXRef-Package-xxHash"; versionInfo = "0.8.3"; downloadLocation = "NOASSERTION"; filesAnalyzed = $false; licenseConcluded = "BSD-2-Clause" },
        @{ name = "cli11"; SPDXID = "SPDXRef-Package-CLI11"; versionInfo = "2.7.0"; downloadLocation = "NOASSERTION"; filesAnalyzed = $false; licenseConcluded = "BSD-3-Clause" },
        @{ name = "tree-sitter"; SPDXID = "SPDXRef-Package-TreeSitter"; versionInfo = "0.26.11"; downloadLocation = "NOASSERTION"; filesAnalyzed = $false; licenseConcluded = "MIT" },
        @{ name = "tree-sitter-c"; SPDXID = "SPDXRef-Package-TreeSitterC"; versionInfo = "0.24.2"; downloadLocation = "NOASSERTION"; filesAnalyzed = $false; licenseConcluded = "MIT" },
        @{ name = "tree-sitter-cpp"; SPDXID = "SPDXRef-Package-TreeSitterCpp"; versionInfo = "0.23.4"; downloadLocation = "NOASSERTION"; filesAnalyzed = $false; licenseConcluded = "MIT" }
    )
}
$sbom | ConvertTo-Json -Depth 8 | Out-File -LiteralPath `
    (Join-Path $stage "repotraverse.spdx.json") -Encoding utf8

Get-FileHash -Algorithm SHA256 $executables.FullName |
    Format-Table -HideTableHeaders Hash, Path |
    Out-File -LiteralPath (Join-Path $stage "SHA256SUMS.txt") -Encoding ascii
if (Test-Path -LiteralPath $archive) {
    Remove-Item -LiteralPath $archive -Force
}
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $archive
Write-Output $archive
