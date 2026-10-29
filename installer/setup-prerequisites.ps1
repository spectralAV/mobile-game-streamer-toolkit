param(
    [ValidateSet("InstallBonjour", "InstallAppleDrivers", "ConfigureFirewall", "CleanupUninstall")]
    [Parameter(Mandatory = $true)]
    [string]$Action,
    [string]$RedistDir = "",
    [string]$ObsDir = "",
    [string]$PluginBinDir = ""
)

$ErrorActionPreference = "Continue"
$logFile = Join-Path $env:TEMP "obs-scrcpy-source-prereqs.log"
$firewallGroup = "OBS scrcpy & AirPlay Source"

function Write-PrereqLog {
    param([string]$Message)
    $stamp = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    $line = "[$stamp] [$Action] $Message"
    Write-Host $line
    Add-Content -LiteralPath $logFile -Value $line -ErrorAction SilentlyContinue
}

function Get-InstalledProductVersion {
    param([string]$ProductName)
    $paths = @(
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*"
    )
    $items = Get-ItemProperty -Path $paths -ErrorAction SilentlyContinue |
        Where-Object { $_.DisplayName -eq $ProductName -and $_.DisplayVersion }
    foreach ($item in $items) {
        try {
            return [version]$item.DisplayVersion
        } catch {}
    }
    return $null
}

function Invoke-MsiSilentInstall {
    param(
        [string]$MsiPath,
        [string]$Label
    )
    if (-not (Test-Path -LiteralPath $MsiPath)) {
        Write-PrereqLog "WARNING: MSI not found: $MsiPath"
        return $false
    }
    $msiLog = Join-Path $env:TEMP ("obs-scrcpy-msi-" + ($Label -replace '\s+', '-') + ".log")
    $args = @(
        "/i", "`"$MsiPath`"",
        "/qn", "/norestart",
        "IAcceptLicense=Yes",
        "REBOOT=ReallySuppress",
        "/L*v", "`"$msiLog`""
    )
    Write-PrereqLog "Installing $Label from $MsiPath..."
    $proc = Start-Process -FilePath "msiexec.exe" -ArgumentList $args -Wait -PassThru -WindowStyle Hidden
    Write-PrereqLog "$Label msiexec exited with code $($proc.ExitCode)"
    return ($proc.ExitCode -in @(0, 1638, 3010))
}

function Ensure-WindowsServiceRunning {
    param([string]$ServiceName)
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($svc) {
        try {
            Set-Service -Name $ServiceName -StartupType Automatic -ErrorAction SilentlyContinue
            if ($svc.Status -ne "Running") {
                Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
            }
            $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
            Write-PrereqLog "Service '$ServiceName' status: $($svc.Status)"
        } catch {
            Write-PrereqLog "WARNING: Could not start '$ServiceName': $($_.Exception.Message)"
        }
    } else {
        Write-PrereqLog "WARNING: Service '$ServiceName' was not found after setup."
    }
}

switch ($Action) {
    "InstallBonjour" {
        $existingVer = Get-InstalledProductVersion -ProductName "Bonjour"
        $bonjourSvc = Get-Service -Name "Bonjour Service" -ErrorAction SilentlyContinue
        if ($existingVer -and ($existingVer -ge [version]"3.1.0.1") -and $bonjourSvc) {
            Write-PrereqLog "Apple Bonjour $existingVer is already installed; skipping MSI."
        } else {
            $bonjourMsi = Join-Path $RedistDir "Bonjour64.msi"
            Invoke-MsiSilentInstall -MsiPath $bonjourMsi -Label "Bonjour" | Out-Null
        }
        Ensure-WindowsServiceRunning -ServiceName "Bonjour Service"
    }

    "InstallAppleDrivers" {
        # 1. Stage & install WHQL Apple USB and Apple Mobile Device Ethernet drivers into DriverStore
        foreach ($relInf in @("drivers\appleusb\AppleUsb.inf", "drivers\netaapl64\netaapl64.Inf")) {
            $infPath = Join-Path $RedistDir $relInf
            if (Test-Path -LiteralPath $infPath) {
                Write-PrereqLog "Staging WHQL driver package: $infPath"
                $pnp = Start-Process -FilePath "pnputil.exe" -ArgumentList @("/add-driver", "`"$infPath`"", "/install") -Wait -PassThru -WindowStyle Hidden
                Write-PrereqLog "pnputil ($relInf) exited with code $($pnp.ExitCode)"
            }
        }

        # 2. Install Apple Mobile Device Support (usbmuxd & iPhone USB trust pairing) if not present
        $amdsVer = Get-InstalledProductVersion -ProductName "Apple Mobile Device Support"
        $amdsSvc = Get-Service -Name "Apple Mobile Device Service" -ErrorAction SilentlyContinue
        if ($amdsVer -and ($amdsVer -ge [version]"20.0.0.35") -and $amdsSvc) {
            Write-PrereqLog "Apple Mobile Device Support $amdsVer is already installed; skipping MSI."
        } else {
            $amdsMsi = Join-Path $RedistDir "AppleMobileDeviceSupport64.msi"
            Invoke-MsiSilentInstall -MsiPath $amdsMsi -Label "AppleMobileDeviceSupport" | Out-Null
        }
        Ensure-WindowsServiceRunning -ServiceName "Apple Mobile Device Service"

        # 3. Provision signed libusb-win32 v1.4.0.2 kernel driver (libusb0.sys) as DEMAND_START
        #    (stays completely dormant unless USB Reverse Tethering is actively enabled in OBS)
        if (-not $PluginBinDir -and $ObsDir) {
            $PluginBinDir = Join-Path $ObsDir "obs-plugins\64bit"
        }
        $bundledSys = Join-Path $PluginBinDir "libusb0.sys"
        $systemSys = Join-Path $env:SystemRoot "System32\drivers\libusb0.sys"
        if ((Test-Path -LiteralPath $bundledSys) -and (-not (Test-Path -LiteralPath $systemSys))) {
            Write-PrereqLog "Copying signed libusb0.sys to $systemSys"
            Copy-Item -LiteralPath $bundledSys -Destination $systemSys -Force
        }
        $libusbSvc = Get-Service -Name "libusb0" -ErrorAction SilentlyContinue
        if ((-not $libusbSvc) -and (Test-Path -LiteralPath $systemSys)) {
            Write-PrereqLog "Registering libusb0 kernel driver service (start=demand)..."
            & sc.exe create libusb0 type= kernel start= demand error= normal binPath= "system32\drivers\libusb0.sys" DisplayName= "libusb-win32 - Kernel Driver, Version 1.4.0.2" | Out-Null
        }

        # 4. Ensure stock Apple USB drivers (usbccgp + AppleLowerFilter + MI_00 Photos + MI_01 AppleUsbMux)
        #    are 100% untouched when OBS is not running (cleans up any leftover state from older builds)
        $obsRunning = Get-Process -Name "obs64", "obs32" -ErrorAction SilentlyContinue
        $helperExe = Join-Path $PluginBinDir "obs-airplay-tether-helper.exe"
        if ((-not $obsRunning) -and (Test-Path -LiteralPath $helperExe)) {
            Write-PrereqLog "Ensuring stock Apple USB driver state via obs-airplay-tether-helper.exe --stop..."
            Start-Process -FilePath $helperExe -ArgumentList @("--stop") -Wait -WindowStyle Hidden | Out-Null
        }
    }

    "ConfigureFirewall" {
        Write-PrereqLog "Configuring Windows Defender Firewall rules ($firewallGroup)..."
        Remove-NetFirewallRule -Group $firewallGroup -ErrorAction SilentlyContinue

        $obsExe = if ($ObsDir) { Join-Path $ObsDir "bin\64bit\obs64.exe" } else { "" }
        if ($obsExe -and (Test-Path -LiteralPath $obsExe)) {
            New-NetFirewallRule `
                -DisplayName "OBS AirPlay Source (OBS Studio TCP)" `
                -Group $firewallGroup `
                -Direction Inbound `
                -Action Allow `
                -Program $obsExe `
                -Protocol TCP `
                -Profile Any `
                -Enabled True | Out-Null

            New-NetFirewallRule `
                -DisplayName "OBS AirPlay Source (OBS Studio UDP)" `
                -Group $firewallGroup `
                -Direction Inbound `
                -Action Allow `
                -Program $obsExe `
                -Protocol UDP `
                -Profile Any `
                -Enabled True | Out-Null
            Write-PrereqLog "Added inbound TCP/UDP firewall rules for $obsExe (Domain, Private, Public)."
        }

        $mdnsExe = "C:\Program Files\Bonjour\mDNSResponder.exe"
        if (Test-Path -LiteralPath $mdnsExe) {
            New-NetFirewallRule `
                -DisplayName "OBS AirPlay Source (Bonjour mDNSResponder)" `
                -Group $firewallGroup `
                -Direction Inbound `
                -Action Allow `
                -Program $mdnsExe `
                -Protocol UDP `
                -Profile Any `
                -Enabled True | Out-Null
        }

        New-NetFirewallRule `
            -DisplayName "OBS AirPlay Source (mDNS UDP 5353)" `
            -Group $firewallGroup `
            -Direction Inbound `
            -Action Allow `
            -Protocol UDP `
            -LocalPort 5353 `
            -Profile Any `
            -Enabled True | Out-Null
        Write-PrereqLog "Added inbound mDNS UDP 5353 firewall rule."
    }

    "CleanupUninstall" {
        Write-PrereqLog "Removing plugin firewall rules ($firewallGroup)..."
        Remove-NetFirewallRule -Group $firewallGroup -ErrorAction SilentlyContinue
    }
}

exit 0

