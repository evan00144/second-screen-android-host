[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$DriverPackagePath,
    [switch]$EnableTestSigning,
    [switch]$TrustTestCertificate,
    [string]$DevgenPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($DriverPackagePath)) {
    $DriverPackagePath = Join-Path $PSScriptRoot '..\native\driver\iddcx\x64\Release\UsbMonitorIddCx'
}
# DevGen is WDK test tooling; this installer is for local developer/test setup only.
if ([string]::IsNullOrWhiteSpace($DevgenPath)) {
    $devgen = Get-ChildItem -Path (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Tools') -Filter 'devgen.exe' -Recurse -File |
        Where-Object { $_.FullName -match '\\x64\\devgen\.exe$' } |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if ($null -eq $devgen) {
        throw 'devgen.exe was not found under the installed Windows Kit tools.'
    }
    $DevgenPath = $devgen.FullName
}
Write-Warning 'This installer uses WDK devgen.exe and is for local developer/test setup only.'

$package = (Resolve-Path -LiteralPath $DriverPackagePath).Path
$infPath = Join-Path $package 'UsbMonitorIddCx.inf'
$certificatePath = Join-Path (Split-Path -Parent $package) 'UsbMonitorIddCx.cer'

foreach ($requiredFile in @($infPath, (Join-Path $package 'UsbMonitorIddCx.dll'), (Join-Path $package 'usbmonitoriddcx.cat'))) {
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

$bcdOutput = (& bcdedit.exe /enum '{current}' 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "bcdedit failed with exit code $LASTEXITCODE."
}

$testSigningEnabled = $bcdOutput -match '(?im)^\s*testsigning\s+Yes\s*$'
if (-not $testSigningEnabled) {
    if (-not $EnableTestSigning) {
        throw 'Windows test-signing is disabled. Re-run with -EnableTestSigning, reboot, then run the script again.'
    }

    if ($PSCmdlet.ShouldProcess('current Windows boot entry', 'enable test-signing')) {
        & bcdedit.exe /set testsigning on
        if ($LASTEXITCODE -ne 0) {
            throw "bcdedit failed with exit code $LASTEXITCODE."
        }
        Write-Warning 'Test-signing enabled. Reboot Windows, then rerun this script with -TrustTestCertificate if the WDK test certificate is not yet trusted.'
    }
    return
}

if (-not (Test-Path -LiteralPath $certificatePath -PathType Leaf)) {
    throw "Test certificate not found: $certificatePath"
}
$certificate = Get-PfxCertificate -FilePath $certificatePath
$thumbprint = ($certificate.Thumbprint -replace '\s', '').ToUpperInvariant()
$trustedCertificate = Get-ChildItem Cert:\LocalMachine\Root |
    Where-Object { ($_.Thumbprint -replace '\s', '').ToUpperInvariant() -eq $thumbprint } |
    Select-Object -First 1
if ($null -eq $trustedCertificate) {
    if (-not $TrustTestCertificate) {
        throw "Test certificate '$($certificate.Subject)' is not trusted in Cert:\LocalMachine\Root. Re-run with -TrustTestCertificate from an elevated prompt."
    }
    $trustCertificate = $PSCmdlet.ShouldProcess($certificatePath, 'trust the WDK test certificate in LocalMachine\Root')
    if ($trustCertificate) {
        Import-Certificate -FilePath $certificatePath -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
        $trustedCertificate = Get-ChildItem Cert:\LocalMachine\Root |
            Where-Object { ($_.Thumbprint -replace '\s', '').ToUpperInvariant() -eq $thumbprint } |
            Select-Object -First 1
        if ($null -eq $trustedCertificate) {
            throw "Test certificate '$($certificate.Subject)' is still not trusted in Cert:\LocalMachine\Root."
        }
    }
}

$instanceId = 'ROOT\DEVGEN\USBMONITORTRANSPORTIDDCX'
if ($PSCmdlet.ShouldProcess($infPath, 'stage the IddCx driver package')) {
    & pnputil.exe /add-driver $infPath
    if ($LASTEXITCODE -ne 0) {
        throw "pnputil failed with exit code $LASTEXITCODE."
    }
}

$existingDevice = Get-PnpDevice -InstanceId $instanceId -ErrorAction SilentlyContinue
if ($null -eq $existingDevice -and $PSCmdlet.ShouldProcess($instanceId, 'create the root-enumerated virtual display device')) {
    & $DevgenPath /add /bus ROOT /instanceid UsbMonitorTransportIddCx /hardwareid 'Root\UsbMonitorTransportIddCx'
    if ($LASTEXITCODE -ne 0) {
        throw "devgen failed with exit code $LASTEXITCODE."
    }
}
elseif ($null -ne $existingDevice) {
    Write-Output "Root device already exists: $instanceId"
}

if ($PSCmdlet.ShouldProcess($infPath, 'bind the staged IddCx driver package')) {
    & pnputil.exe /add-driver $infPath /install
    if ($LASTEXITCODE -ne 0) {
        throw "pnputil failed with exit code $LASTEXITCODE."
    }
    Write-Output 'Root device and driver package installed. Verify with: pnputil /enum-devices /class Display'
}
