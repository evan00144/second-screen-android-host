[CmdletBinding()]
param(
    [string]$HostExecutable = (Join-Path $PSScriptRoot '..\build\host\phase16\second-screen-host.exe'),
    [int]$Port = 5000,
    [switch]$RequireHost,
    [switch]$RequireAndroid,
    [switch]$RequireCurrentDriver
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$script:failed = $false
$metadata = $null

function Report-Check {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [bool]$Passed,
        [Parameter(Mandatory = $true)] [string]$Detail,
        [switch]$Optional
    )

    if ($Passed) {
        Write-Host "[PASS] $Name - $Detail" -ForegroundColor Green
        return
    }

    $prefix = if ($Optional) { '[WARN]' } else { '[FAIL]' }
    $color = if ($Optional) { 'Yellow' } else { 'Red' }
    Write-Host "$prefix $Name - $Detail" -ForegroundColor $color
    if (-not $Optional) {
        $script:failed = $true
    }
}

function Get-CommandPath {
    param([Parameter(Mandatory = $true)] [string]$Name)
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        return $null
    }
    return $command.Source
}

$metadataPath = Join-Path $root 'version.json'
if (Test-Path -LiteralPath $metadataPath -PathType Leaf) {
    $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
    Report-Check 'release metadata' ($metadata.phase -eq 16) ("phase={0} version={1} driver={2}" -f $metadata.phase, $metadata.version, $metadata.driverVersion)
}
else {
    Report-Check 'release metadata' $false "missing $metadataPath"
}

$packagePath = Join-Path $root 'native\driver\iddcx\x64\Release\UsbMonitorIddCx'
$packageFiles = @(
    (Join-Path $packagePath 'UsbMonitorIddCx.inf'),
    (Join-Path $packagePath 'UsbMonitorIddCx.dll'),
    (Join-Path $packagePath 'usbmonitoriddcx.cat')
)
$missingPackageFiles = @($packageFiles | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
Report-Check 'driver package' ($missingPackageFiles.Count -eq 0) $(if ($missingPackageFiles.Count -eq 0) { $packagePath } else { "missing: $($missingPackageFiles -join ', ')" })
if ($null -ne $metadata -and $missingPackageFiles.Count -eq 0) {
    $packageInf = Get-Content -LiteralPath (Join-Path $packagePath 'UsbMonitorIddCx.inf') -Raw
    $packageVersionMatches = $packageInf -match ('DriverVer=.*,' + [regex]::Escape([string]$metadata.driverVersion))
    Report-Check 'driver package version' $packageVersionMatches $(if ($packageVersionMatches) { $metadata.driverVersion } else { 'version.json does not match packaged DriverVer' })
}

$instanceId = 'ROOT\DEVGEN\USBMONITORTRANSPORTIDDCX'
$pnpCommand = Get-CommandPath 'Get-PnpDevice'
if ($null -eq $pnpCommand) {
    Report-Check 'PnP cmdlet' $false 'Get-PnpDevice is unavailable'
}
else {
    $device = Get-PnpDevice -InstanceId $instanceId -ErrorAction SilentlyContinue
    if ($null -eq $device) {
        Report-Check 'virtual display' $false "device not found: $instanceId"
    }
    else {
        Report-Check 'virtual display' ($device.Status -eq 'OK') ("status={0} class={1}" -f $device.Status, $device.Class)
    }
}

$pnputil = Get-CommandPath 'pnputil.exe'
if ($null -eq $pnputil) {
    Report-Check 'pnputil' $false 'pnputil.exe is unavailable'
}
else {
    $driverOutput = @(& $pnputil /enum-devices /instanceid $instanceId /drivers 2>&1)
    $driverExitCode = $LASTEXITCODE
    $driverSummary = @($driverOutput | Where-Object { $_ -match 'Driver Name:|Driver Version:|Driver Status:' } | ForEach-Object { $_.ToString().Trim() })
    Report-Check 'installed driver' ($driverExitCode -eq 0 -and $driverSummary.Count -gt 0) $(if ($driverSummary.Count -gt 0) { $driverSummary -join '; ' } else { "pnputil exit=$driverExitCode" })
    if ($null -ne $metadata -and $driverExitCode -eq 0) {
        $installedVersionMatches = ($driverOutput -join "`n") -match [regex]::Escape([string]$metadata.driverVersion)
        Report-Check 'installed driver version' $installedVersionMatches $(if ($installedVersionMatches) { $metadata.driverVersion } else { "expected $($metadata.driverVersion)" }) -Optional:(-not $RequireCurrentDriver)
    }
}

$hostPath = Resolve-Path -LiteralPath $HostExecutable -ErrorAction SilentlyContinue
Report-Check 'host binary' ($null -ne $hostPath) $(if ($null -ne $hostPath) { $hostPath.Path } else { "missing $HostExecutable" })

$hostProcesses = @(Get-CimInstance Win32_Process -Filter "Name='second-screen-host.exe'" -ErrorAction SilentlyContinue)
if ($hostProcesses.Count -eq 0) {
    Report-Check 'host process' $false 'second-screen-host.exe is not running' -Optional:(-not $RequireHost)
}
else {
    Report-Check 'host process' $true ("count={0}" -f $hostProcesses.Count)
}

$tcpConnections = @(Get-NetTCPConnection -LocalAddress '127.0.0.1' -LocalPort $Port -State Listen -ErrorAction SilentlyContinue)
if ($tcpConnections.Count -eq 0) {
    Report-Check 'host port' $false "127.0.0.1:$Port is not listening" -Optional:(-not $RequireHost)
}
else {
    Report-Check 'host port' $true "127.0.0.1:$Port is listening"
}

$adb = Get-CommandPath 'adb.exe'
if ($null -eq $adb) {
    Report-Check 'adb' $false 'adb.exe is unavailable' -Optional:(-not $RequireAndroid)
}
else {
    $adbState = @(& $adb get-state 2>&1)
    $adbStateExitCode = $LASTEXITCODE
    $adbReady = $adbStateExitCode -eq 0 -and ($adbState -join ' ') -match 'device'
    if (-not $adbReady) {
        Report-Check 'Android device' $false "adb state unavailable: $($adbState -join ' ')" -Optional:(-not $RequireAndroid)
    }
    else {
        Report-Check 'Android device' $true 'adb device ready'
        & $adb reverse "tcp:$Port" "tcp:$Port" | Out-Null
        $reverseExitCode = $LASTEXITCODE
        Report-Check 'adb reverse' ($reverseExitCode -eq 0) "tcp:$Port -> tcp:$Port"

        $reverseList = @(& $adb reverse --list 2>&1)
        $reverseConfigured = ($reverseList -join "`n") -match "tcp:$Port\s+tcp:$Port"
        Report-Check 'reverse mapping' $reverseConfigured $(if ($reverseConfigured) { "tcp:$Port tcp:$Port" } else { $reverseList -join '; ' })

        $recentLogs = @(& $adb logcat -d -t 300 -v brief 'SecondScreenStream:I' 'SecondScreenBuild:I' '*:S' 2>&1)
        $hasStreamLog = @($recentLogs | Where-Object { $_ -match 'SecondScreenStream|SecondScreenBuild' }).Count -gt 0
        Report-Check 'Android stream log' $hasStreamLog $(if ($hasStreamLog) { 'recent SecondScreenStream/SecondScreenBuild entries found' } else { 'no recent SecondScreenStream/SecondScreenBuild entries' }) -Optional:(-not $RequireAndroid)
    }
}

if ($script:failed) {
    Write-Host 'Phase 16 check: FAILED' -ForegroundColor Red
    exit 1
}

Write-Host 'Phase 16 check: PASS' -ForegroundColor Green
exit 0
