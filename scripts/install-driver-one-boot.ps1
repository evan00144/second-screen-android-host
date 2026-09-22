[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$DriverPackagePath,
    [string]$DevgenPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

<##
.SYNOPSIS
Installs the local IddCx package during the current Windows boot only.

.NOTES
Before running: Advanced startup -> Startup Settings -> Restart -> press 7/F7
(Disable driver signature enforcement). This script cannot detect that transient
boot choice. Run from an elevated PowerShell window. No BCD setting is changed.
#>

if ([string]::IsNullOrWhiteSpace($DriverPackagePath)) {
    $DriverPackagePath = Join-Path $PSScriptRoot '..\native\driver\iddcx\x64\Release\UsbMonitorIddCx'
}

if ([string]::IsNullOrWhiteSpace($DevgenPath)) {
    $toolsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Tools'
    $devgen = Get-ChildItem -Path $toolsRoot -Filter 'devgen.exe' -Recurse -File |
        Where-Object { $_.FullName -match '\\x64\\devgen\.exe$' } |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if ($null -eq $devgen) {
        throw 'devgen.exe was not found under the installed Windows Kit tools.'
    }
    $DevgenPath = $devgen.FullName
}
$package = (Resolve-Path -LiteralPath $DriverPackagePath).Path

$certificatePath = Join-Path (Split-Path -Parent $package) 'UsbMonitorIddCx.cer'
$infPath = Join-Path $package 'UsbMonitorIddCx.inf'

foreach ($requiredFile in @(
        $infPath,
        (Join-Path $package 'UsbMonitorIddCx.dll'),
        (Join-Path $package 'usbmonitoriddcx.cat'),
        $certificatePath)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Driver package is incomplete: $requiredFile"
    }
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$administrator = [Security.Principal.WindowsBuiltInRole]::Administrator
if (-not $principal.IsInRole($administrator)) {
    throw 'Run this script from an elevated PowerShell prompt.'
}

$certificate = Get-PfxCertificate -FilePath $certificatePath
$thumbprint = ($certificate.Thumbprint -replace '\s', '').ToUpperInvariant()
$trustedCertificate = Get-ChildItem Cert:\LocalMachine\Root |
    Where-Object { ($_.Thumbprint -replace '\s', '').ToUpperInvariant() -eq $thumbprint } |
    Select-Object -First 1
if ($null -eq $trustedCertificate) {
    Write-Output "Trusting driver certificate: $($certificate.Subject)"
    Import-Certificate -FilePath $certificatePath -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    $trustedCertificate = Get-ChildItem Cert:\LocalMachine\Root |
        Where-Object { ($_.Thumbprint -replace '\s', '').ToUpperInvariant() -eq $thumbprint } |
        Select-Object -First 1
    if ($null -eq $trustedCertificate) {
        throw "Driver certificate '$($certificate.Subject)' was not added to Cert:\LocalMachine\Root."
    }
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [Parameter(Mandatory = $true)] [string[]]$ArgumentList,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    & $FilePath @ArgumentList
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "$Description failed with exit code $exitCode."
    }
}

Write-Warning 'One-boot mode: this install is valid only for the current Windows boot after Startup Settings option 7/F7.'
Write-Output "Using driver package: $package"
Write-Output "Using devgen: $DevgenPath"

if ($PSCmdlet.ShouldProcess($infPath, 'stage the IddCx driver package')) {
    Invoke-Native -FilePath 'pnputil.exe' -ArgumentList @('/add-driver', $infPath) -Description 'pnputil staging'
}

$instanceId = 'ROOT\DEVGEN\USBMONITORTRANSPORTIDDCX'
$existingDevice = Get-PnpDevice -InstanceId $instanceId -ErrorAction SilentlyContinue
if ($null -eq $existingDevice) {
    if ($PSCmdlet.ShouldProcess($instanceId, 'create the root-enumerated virtual display device')) {
        Invoke-Native -FilePath $DevgenPath -ArgumentList @(
            '/add',
            '/bus', 'ROOT',
            '/instanceid', 'UsbMonitorTransportIddCx',
            '/hardwareid', 'Root\UsbMonitorTransportIddCx'
        ) -Description 'devgen root-device creation'
    }
}
else {
    Write-Output "Root device already exists: $instanceId"
}

if ($PSCmdlet.ShouldProcess($infPath, 'bind the staged IddCx driver package')) {
    Invoke-Native -FilePath 'pnputil.exe' -ArgumentList @('/add-driver', $infPath, '/install') -Description 'pnputil driver binding'
    Write-Output 'Root device and driver package installed for the current boot.'
    Write-Output "Verify with: pnputil.exe /enum-devices /instanceid '$instanceId'"
}

