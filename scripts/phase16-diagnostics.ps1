[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [string]$HostLogPath,
    [int]$LogcatLines = 2000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root "diagnostics\phase16-$timestamp"
}
$output = (New-Item -ItemType Directory -Force -Path $OutputDirectory).FullName

function Save-Text {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [scriptblock]$Action
    )

    try {
        & $Action 2>&1 | Out-File -LiteralPath $Path -Encoding utf8
    }
    catch {
        $_ | Out-File -LiteralPath $Path -Encoding utf8
    }
}

Copy-Item -LiteralPath (Join-Path $root 'version.json') -Destination (Join-Path $output 'version.json') -Force

if ([string]::IsNullOrWhiteSpace($HostLogPath)) {
    $candidateLogs = @(Get-ChildItem -LiteralPath $root -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^phase.*\.log$' } |
        Sort-Object LastWriteTime -Descending)
    if ($candidateLogs.Count -gt 0) {
        $HostLogPath = $candidateLogs[0].FullName
    }
}
if (-not [string]::IsNullOrWhiteSpace($HostLogPath) -and (Test-Path -LiteralPath $HostLogPath -PathType Leaf)) {
    Get-Content -LiteralPath $HostLogPath -Tail 10000 | Out-File -LiteralPath (Join-Path $output 'host-tail.log') -Encoding utf8
}
else {
    'No host log supplied or found.' | Out-File -LiteralPath (Join-Path $output 'host-tail.log') -Encoding utf8
}

Save-Text (Join-Path $output 'git.txt') {
    git -C $root status --short --branch
    git -C $root log -1 --format='%H%n%ad%n%s' --date=iso
}

Save-Text (Join-Path $output 'driver-device.txt') {
    pnputil.exe /enum-devices /instanceid 'ROOT\DEVGEN\USBMONITORTRANSPORTIDDCX' /drivers
}

Save-Text (Join-Path $output 'driver-package.txt') {
    Get-Item (Join-Path $root 'native\driver\iddcx\x64\Release\UsbMonitorIddCx\*') |
        Select-Object FullName, Length, LastWriteTime
    Get-Content (Join-Path $root 'native\driver\iddcx\UsbMonitorIddCx.inf') |
        Select-String 'DriverVer|CatalogFile'
}

Save-Text (Join-Path $output 'host-process.txt') {
    Get-CimInstance Win32_Process -Filter "Name='second-screen-host.exe'" |
        Select-Object ProcessId, ExecutablePath, CommandLine
    Get-NetTCPConnection -LocalAddress '127.0.0.1' -LocalPort 5000 -ErrorAction SilentlyContinue |
        Select-Object LocalAddress, LocalPort, State, OwningProcess
}

Save-Text (Join-Path $output 'system.txt') {
    Get-CimInstance Win32_OperatingSystem |
        Select-Object Caption, Version, BuildNumber, OSArchitecture
    Get-CimInstance Win32_VideoController |
        Select-Object Name, DriverVersion, VideoModeDescription
}

$adb = Get-Command adb.exe -ErrorAction SilentlyContinue
if ($null -ne $adb) {
    Save-Text (Join-Path $output 'adb-devices.txt') {
        adb.exe devices -l
        adb.exe reverse --list
    }
    Save-Text (Join-Path $output 'android-properties.txt') {
        adb.exe shell getprop ro.product.manufacturer
        adb.exe shell getprop ro.product.model
        adb.exe shell getprop ro.build.version.release
        adb.exe shell dumpsys package com.secondscreen.android | Select-String 'versionCode|versionName'
    }
    Save-Text (Join-Path $output 'android-logcat.txt') {
        adb.exe logcat -d -t $LogcatLines -v threadtime 'SecondScreenStream:I' 'SecondScreenBuild:I' '*:S'
    }
}
else {
    'adb.exe not found on PATH.' | Out-File -LiteralPath (Join-Path $output 'adb-devices.txt') -Encoding utf8
}

$archive = "$output.zip"
if (Test-Path -LiteralPath $archive) {
    throw "Diagnostic archive already exists: $archive"
}
Compress-Archive -Path (Join-Path $output '*') -DestinationPath $archive -CompressionLevel Optimal
Write-Output "Diagnostic bundle: $archive"
