[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Config,
    [string]$Name = "Repotraverse",
    [string]$Executable = (Join-Path $PSScriptRoot "..\bin\repotraverse.exe")
)

$ErrorActionPreference = "Stop"
$exe = (Resolve-Path -LiteralPath $Executable).Path
$configPath = (Resolve-Path -LiteralPath $Config).Path
if (Get-Service -Name $Name -ErrorAction SilentlyContinue) {
    throw "Service '$Name' already exists. Uninstall it before reinstalling."
}
$command = '"{0}" service --config "{1}"' -f $exe, $configPath
New-Service -Name $Name -BinaryPathName $command -DisplayName "Repotraverse" `
    -Description "Local C/C++ element history analysis service" `
    -StartupType Automatic
if (-not [System.Diagnostics.EventLog]::SourceExists("Repotraverse")) {
    New-EventLog -LogName Application -Source "Repotraverse"
}
Start-Service -Name $Name
Get-Service -Name $Name
