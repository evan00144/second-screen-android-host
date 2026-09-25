[CmdletBinding()]
param(
    [ValidateSet('Windows', 'Android', 'All')]
    [string]$Target = 'Windows',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipHost,
    [switch]$SkipDriver,
    [switch]$SkipAndroid,
    [switch]$InstallDriver,
    [switch]$OneBoot,
    [switch]$InstallAndroid,
    [switch]$VerifyRuntime,
    [switch]$EnableTestSigning,
    [switch]$TrustTestCertificate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$metadata = Get-Content -LiteralPath (Join-Path $root 'version.json') -Raw | ConvertFrom-Json
$hostBuildDirectory = Join-Path $root 'build\host\phase16'
$hostExecutable = Join-Path $hostBuildDirectory 'second-screen-host.exe'
$driverProject = Join-Path $root 'native\driver\iddcx\UsbMonitorIddCx.vcxproj'
$driverPackage = Join-Path $root "native\driver\iddcx\x64\$Configuration\UsbMonitorIddCx"
$androidRoot = Join-Path $root 'apps\android'
$buildWindows = $Target -in @('Windows', 'All')
$buildAndroid = $Target -in @('Android', 'All')

if ($InstallDriver -and -not $buildWindows) {
    throw '-InstallDriver requires -Target Windows or -Target All.'
}
if ($OneBoot -and -not $InstallDriver) {
    throw '-OneBoot requires -InstallDriver.'
}
if ($OneBoot -and ($EnableTestSigning -or $TrustTestCertificate)) {
    throw '-OneBoot cannot be combined with -EnableTestSigning or -TrustTestCertificate.'
}
if ($InstallAndroid -and -not $buildAndroid) {
    throw '-InstallAndroid requires -Target Android or -Target All.'
}
if ($InstallAndroid -and $SkipAndroid) {
    throw '-InstallAndroid cannot be combined with -SkipAndroid.'
}

$testSigningWasDisabled = $false
if ($InstallDriver -and -not $OneBoot) {
    $bcdOutput = (& bcdedit.exe /enum '{current}' 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "bcdedit failed with exit code $LASTEXITCODE. Run from an elevated PowerShell prompt."
    }
    $testSigningWasDisabled = $bcdOutput -notmatch '(?im)^\s*testsigning\s+Yes\s*$'
    if ($testSigningWasDisabled -and -not $EnableTestSigning) {
        throw 'Windows test-signing is disabled. Re-run with -EnableTestSigning, reboot, then rerun the Windows target.'
    }
    if ($testSigningWasDisabled -and $EnableTestSigning) {
        $secureBootEnabled = $false
        try {
            $secureBootEnabled = [bool](Confirm-SecureBootUEFI -ErrorAction Stop)
        }
        catch {
            $secureBootState = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -Name UEFISecureBootEnabled -ErrorAction SilentlyContinue
            $secureBootEnabled = $null -ne $secureBootState -and [int]$secureBootState.UEFISecureBootEnabled -eq 1
        }
        if ($secureBootEnabled) {
            throw 'Secure Boot blocks test-signing changes. Use -OneBoot after Advanced startup > Startup Settings > Disable driver signature enforcement, or disable Secure Boot in UEFI.'
        }
    }
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [Parameter(Mandatory = $true)] [string[]]$ArgumentList,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    Write-Host "[RUN] $Description" -ForegroundColor Cyan
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

function Resolve-CMake {
    $direct = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($null -ne $direct) {
        return $direct.Source
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $installationPath = (& $vswhere -latest -products * -property installationPath | Select-Object -First 1).Trim()
        if (-not [string]::IsNullOrWhiteSpace($installationPath)) {
            $bundled = Join-Path $installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $bundled -PathType Leaf) {
                return $bundled
            }
        }
    }
    throw 'cmake.exe was not found. Install CMake or the Visual Studio CMake workload.'
}

function Resolve-Gradle {
    foreach ($name in @('gradle.bat', 'gradle.exe', 'gradle')) {
        $direct = Get-Command $name -ErrorAction SilentlyContinue
        if ($null -ne $direct) {
            return $direct.Source
        }
    }

    $wrapperRoot = Join-Path $env:USERPROFILE '.gradle\wrapper\dists'
    $cached = Get-ChildItem -Path $wrapperRoot -Filter 'gradle.bat' -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -ne $cached) {
        return $cached.FullName
    }
    throw 'Gradle was not found. Add gradle to PATH or commit a Gradle wrapper under apps/android.'
}

function Resolve-MSBuild {
    $direct = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($null -ne $direct) {
        return $direct.Source
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'MSBuild was not found. Install Visual Studio 2022 with C++ and WDK workloads.'
    }

    $resolved = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($resolved) -or -not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw 'MSBuild was not found through vswhere.'
    }
    return $resolved.Trim()
}

Write-Host ("Phase {0} release {1} ({2}) target={3}" -f $metadata.phase, $metadata.version, $metadata.hostBuild, $Target) -ForegroundColor White

if ($buildWindows -and -not $SkipHost) {
    $cmake = Resolve-CMake
    if (-not (Test-Path -LiteralPath (Join-Path $hostBuildDirectory 'CMakeCache.txt') -PathType Leaf)) {
        Invoke-Native $cmake @('-S', (Join-Path $root 'native\streamer'), '-B', $hostBuildDirectory, '-A', 'x64') 'configure host'
    }
    Invoke-Native $cmake @('--build', $hostBuildDirectory, '--config', $Configuration) 'build host'
    if (-not (Test-Path -LiteralPath $hostExecutable -PathType Leaf)) {
        $hostExecutable = (Get-ChildItem -LiteralPath $hostBuildDirectory -Filter 'second-screen-host.exe' -Recurse -File |
            Select-Object -First 1).FullName
    }
    if ([string]::IsNullOrWhiteSpace($hostExecutable) -or -not (Test-Path -LiteralPath $hostExecutable -PathType Leaf)) {
        throw 'Host build completed without producing second-screen-host.exe.'
    }
    Write-Host "[PASS] host: $hostExecutable" -ForegroundColor Green
}

if ($buildWindows -and -not $SkipDriver) {
    $msbuild = Resolve-MSBuild
    $driverArguments = @(
        $driverProject,
        '/m',
        "/p:Configuration=$Configuration",
        '/p:Platform=x64'
    )
    $infVerifier = Get-ChildItem -Path (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') -Filter 'InfVerif.dll' -Recurse -File -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $infVerifier) {
        Write-Warning 'WDK InfVerif.dll is missing; skipping package verification. Inf2Cat and signing still run.'
        $driverArguments += '/p:SkipPackageVerification=true'
    }
    Invoke-Native $msbuild $driverArguments 'build driver'
    if (-not (Test-Path -LiteralPath $driverPackage -PathType Container)) {
        throw "Driver package directory was not produced: $driverPackage"
    }
    Write-Host "[PASS] driver package: $driverPackage" -ForegroundColor Green
}

if ($buildAndroid -and -not $SkipAndroid) {
    $gradleWrapper = Join-Path $androidRoot 'gradlew.bat'
    if (Test-Path -LiteralPath $gradleWrapper -PathType Leaf) {
        $gradle = $gradleWrapper
    }
    else {
        $gradle = Resolve-Gradle
    }
    Push-Location $androidRoot
    try {
        Invoke-Native $gradle @(':app:assembleDebug') 'build Android APK'
    }
    finally {
        Pop-Location
    }
    $apk = Join-Path $androidRoot 'app\build\outputs\apk\debug\app-debug.apk'
    if (-not (Test-Path -LiteralPath $apk -PathType Leaf)) {
        throw "Android build completed without producing $apk"
    }
    Write-Host "[PASS] Android APK: $apk" -ForegroundColor Green
}

if ($InstallDriver) {
    if ($SkipDriver) {
        throw '-InstallDriver requires driver build; remove -SkipDriver.'
    }
    $installScript = Join-Path $PSScriptRoot $(if ($OneBoot) { 'install-driver-one-boot.ps1' } else { 'install-driver.ps1' })
    $installParameters = @{ DriverPackagePath = $driverPackage }
    if (-not $OneBoot -and $EnableTestSigning) { $installParameters['EnableTestSigning'] = $true }
    if (-not $OneBoot -and $TrustTestCertificate) { $installParameters['TrustTestCertificate'] = $true }
    & $installScript @installParameters
    if ($LASTEXITCODE -ne 0) {
        throw "driver installation failed with exit code $LASTEXITCODE."
    }
    if ($testSigningWasDisabled) {
        Write-Warning 'Test-signing was enabled. Reboot Windows, then rerun with -InstallDriver -TrustTestCertificate.'
        return
    }
}

if ($InstallAndroid) {
    $apk = Join-Path $androidRoot 'app\build\outputs\apk\debug\app-debug.apk'
    $adbForInstall = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($null -eq $adbForInstall) {
        throw 'adb.exe was not found; cannot install the Android APK.'
    }
    $adbState = @(& $adbForInstall.Source get-state 2>&1)
    if ($LASTEXITCODE -ne 0 -or ($adbState -join ' ') -notmatch 'device') {
        throw 'No authorized Android device is available for APK installation.'
    }
    Invoke-Native $adbForInstall.Source @('install', '-r', $apk) 'install Android APK'
    Write-Host "[PASS] Android installed: $apk" -ForegroundColor Green
}

$adb = Get-Command adb.exe -ErrorAction SilentlyContinue
if ($null -ne $adb) {
    & $adb.Source reverse tcp:5000 tcp:5000 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host '[PASS] adb reverse: tcp:5000 -> tcp:5000' -ForegroundColor Green
    }
    else {
        Write-Warning 'adb reverse skipped: no authorized Android device is available.'
    }
}
else {
    Write-Warning 'adb.exe not found; Android runtime verification skipped.'
}

if ($VerifyRuntime) {
    $checkScript = Join-Path $PSScriptRoot 'phase16-check.ps1'
    $checkArguments = @('-HostExecutable', $hostExecutable, '-RequireAndroid')
    if ($buildWindows) { $checkArguments += '-RequireHost' }
    if ($InstallDriver) { $checkArguments += '-RequireCurrentDriver' }
    & $checkScript @checkArguments
    if ($LASTEXITCODE -ne 0) {
        throw 'runtime verification failed.'
    }
}

Write-Host 'Phase 16 release flow: PASS' -ForegroundColor Green
if ($buildWindows) {
    Write-Host "Host: $hostExecutable"
    Write-Host "Start: $hostExecutable --width 1920 --height 1200 --fps 60 --bitrate 8000000"
}
if ($buildAndroid) {
    Write-Host "APK: $(Join-Path $androidRoot 'app\build\outputs\apk\debug\app-debug.apk')"
}
