[CmdletBinding()]
param([string]$Name = "Repotraverse")

$ErrorActionPreference = "Stop"
$service = Get-Service -Name $Name -ErrorAction SilentlyContinue
if (-not $service) {
    return
}
if ($service.Status -ne "Stopped") {
    Stop-Service -Name $Name
    $service.WaitForStatus("Stopped", [TimeSpan]::FromSeconds(30))
}
sc.exe delete $Name | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Failed to delete service '$Name'."
}
if ([System.Diagnostics.EventLog]::SourceExists("Repotraverse")) {
    Remove-EventLog -Source "Repotraverse"
}
