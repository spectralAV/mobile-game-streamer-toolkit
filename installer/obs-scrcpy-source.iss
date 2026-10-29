; OBS scrcpy Source Plugin - Inno Setup Script
; High-performance native Android scrcpy & Apple AirPlay input sources for OBS Studio

#ifndef MyAppVersion
  #define MyAppVersion "0.3.6"
#endif

#ifndef MyAppName
  #define MyAppName "OBS scrcpy Source Plugin"
#endif

#ifndef MyAppPublisher
  #define MyAppPublisher "spectralAV"
#endif

#ifndef MyAppURL
  #define MyAppURL "https://github.com/spectralAV/mobile-game-streamer-toolkit"
#endif

#ifndef MyAppId
  #define MyAppId "obs-scrcpy-source"
#endif

#ifndef StagingDir
  #define StagingDir "..\build\installer-staging"
#endif

#ifndef OutputDir
  #define OutputDir "..\release"
#endif

#ifndef OutputBaseFilename
  #define OutputBaseFilename "obs-scrcpy-source-" + MyAppVersion + "-windows-x64-Installer"
#endif

#ifndef SetupIconFile
  #define SetupIconFile "..\vendor\scrcpy-4.0\app\data\icon.ico"
#endif

[Setup]
AppId={{8B19E0E7-3A39-4467-8C52-6C48B257E449}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={code:GetOBSInstallDir}
DirExistsWarning=no
EnableDirDoesntExistWarning=no
AppendDefaultDirName=no
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
#if FileExists(SetupIconFile)
SetupIconFile={#SetupIconFile}
#endif
UninstallDisplayIcon={app}\obs-plugins\64bit\obs-scrcpy-source.dll
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog commandline
WizardStyle=modern
DisableWelcomePage=no
CloseApplications=no
InfoAfterFile=install-instructions.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Full Out-of-the-Box Installation (Recommended - Plugin, scrcpy 4.1, AirPlay, Bonjour, Apple USB Drivers & Firewall Rules)"
Name: "compact"; Description: "Android Only Installation (Plugin + scrcpy 4.1 runtime only)"
Name: "custom"; Description: "Custom Installation"; Flags: iscustom

[Components]
Name: "core"; Description: "OBS scrcpy Source Plugin (Core + FFmpeg D3D11VA runtime)"; Types: full compact custom; Flags: fixed
Name: "scrcpy"; Description: "scrcpy 4.1 Runtime & ADB Tools (Installs to Program Files\scrcpy\4.1)"; Types: full compact custom
Name: "airplay"; Description: "Apple AirPlay Receiver & iPhone USB Reverse Tethering Runtime (iOS/iPadOS/macOS)"; Types: full custom
Name: "airplay\bonjour"; Description: "Apple Bonjour Service v3.1 (Required for AirPlay device discovery)"; Types: full custom
Name: "airplay\apple_drivers"; Description: "Apple Mobile Device & USB Reverse Tethering Drivers (AMDS + WHQL USB/NCM + libusb0)"; Types: full custom
Name: "airplay\firewall"; Description: "Windows Defender Firewall Rules (Allow OBS AirPlay & mDNS over Wi-Fi and USB)"; Types: full custom
Name: "symbols"; Description: "Development Debug Symbols (.pdb)"; Types: custom

[Files]
; Core plugin DLL & PDB -> {app}\obs-plugins\64bit
Source: "{#StagingDir}\obs-plugins\64bit\obs-scrcpy-source.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion; Components: core
Source: "{#StagingDir}\obs-plugins\64bit\obs-scrcpy-source.pdb"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion skipifsourcedoesntexist; Components: symbols

; Core FFmpeg runtime DLLs (Full shared build with D3D11VA hardware acceleration) -> {app}\obs-plugins\64bit
Source: "{#StagingDir}\obs-plugins\64bit\avcodec-62.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion; Components: core
Source: "{#StagingDir}\obs-plugins\64bit\avutil-60.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion; Components: core
Source: "{#StagingDir}\obs-plugins\64bit\swresample-6.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion; Components: core

; AirPlay runtime & iPhone USB Reverse Tethering helper -> {app}\obs-plugins\64bit
Source: "{#StagingDir}\obs-plugins\64bit\airplay2dll.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion skipifsourcedoesntexist; Components: airplay
Source: "{#StagingDir}\obs-plugins\64bit\swscale-9.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion skipifsourcedoesntexist; Components: airplay
Source: "{#StagingDir}\obs-plugins\64bit\obs-airplay-tether-helper.exe"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion skipifsourcedoesntexist; Components: airplay
Source: "{#StagingDir}\obs-plugins\64bit\obs-airplay-tether-helper.pdb"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion skipifsourcedoesntexist; Components: symbols
Source: "{#StagingDir}\obs-plugins\64bit\libusb0.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion skipifsourcedoesntexist; Components: airplay
Source: "{#StagingDir}\obs-plugins\64bit\libusb0.sys"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion skipifsourcedoesntexist; Components: airplay

; Temporary prerequisite redistributables (auto-deleted after installation) -> {tmp}\obs-airplay-redist
Source: "{#StagingDir}\redist\setup-prerequisites.ps1"; DestDir: "{tmp}\obs-airplay-redist"; Flags: ignoreversion deleteafterinstall; Components: airplay
Source: "{#StagingDir}\redist\Bonjour64.msi"; DestDir: "{tmp}\obs-airplay-redist"; Flags: ignoreversion deleteafterinstall skipifsourcedoesntexist; Components: airplay\bonjour
Source: "{#StagingDir}\redist\AppleMobileDeviceSupport64.msi"; DestDir: "{tmp}\obs-airplay-redist"; Flags: ignoreversion deleteafterinstall skipifsourcedoesntexist; Components: airplay\apple_drivers
Source: "{#StagingDir}\redist\drivers\*"; DestDir: "{tmp}\obs-airplay-redist\drivers"; Flags: ignoreversion recursesubdirs createallsubdirs deleteafterinstall skipifsourcedoesntexist; Components: airplay\apple_drivers

; Plugin data & locale resources -> {app}\data\obs-plugins\obs-scrcpy-source
Source: "{#StagingDir}\data\obs-plugins\obs-scrcpy-source\*"; DestDir: "{app}\data\obs-plugins\obs-scrcpy-source"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: core

; scrcpy 4.1 runtime and ADB tools -> C:\Program Files\scrcpy\4.1
Source: "{#StagingDir}\scrcpy-4.1\*"; DestDir: "{code:GetScrcpyInstallDir}"; Flags: ignoreversion recursesubdirs createallsubdirs uninsneveruninstall; Components: scrcpy

[InstallDelete]
Type: files; Name: "{app}\obs-plugins\64bit\install-filter.exe"
Type: files; Name: "{app}\obs-plugins\64bit\msys-2.0.dll"
Type: files; Name: "{app}\obs-plugins\64bit\avcodec-58.dll"
Type: files; Name: "{app}\obs-plugins\64bit\avutil-56.dll"
Type: files; Name: "{app}\obs-plugins\64bit\swscale-5.dll"

[UninstallDelete]
Type: files; Name: "{app}\obs-plugins\64bit\airplay2dll.dll"
Type: files; Name: "{app}\obs-plugins\64bit\swscale-9.dll"
Type: files; Name: "{app}\obs-plugins\64bit\obs-airplay-tether-helper.exe"
Type: files; Name: "{app}\obs-plugins\64bit\obs-airplay-tether-helper.pdb"
Type: files; Name: "{app}\obs-plugins\64bit\libusb0.dll"
Type: files; Name: "{app}\obs-plugins\64bit\libusb0.sys"
Type: files; Name: "{app}\obs-plugins\64bit\install-filter.exe"
Type: files; Name: "{app}\obs-plugins\64bit\msys-2.0.dll"
Type: files; Name: "{app}\obs-plugins\64bit\avcodec-58.dll"
Type: files; Name: "{app}\obs-plugins\64bit\avutil-56.dll"
Type: files; Name: "{app}\obs-plugins\64bit\swscale-5.dll"
Type: filesandordirs; Name: "{app}\data\obs-plugins\obs-scrcpy-source"

[Run]
Filename: "powershell.exe"; Parameters: "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -Command ""$ErrorActionPreference = 'SilentlyContinue'; $a = New-ScheduledTaskAction -Execute '{app}\obs-plugins\64bit\obs-airplay-tether-helper.exe' -Argument '--run'; $p = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest; Register-ScheduledTask -TaskName 'OBS-AirPlay-Tether-Helper' -Action $a -Principal $p -Force | Out-Null"""; Flags: runhidden waituntilterminated; Components: airplay; StatusMsg: "Registering AirPlay USB Reverse Tethering helper..."
Filename: "powershell.exe"; Parameters: "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File ""{tmp}\obs-airplay-redist\setup-prerequisites.ps1"" -Action InstallBonjour -RedistDir ""{tmp}\obs-airplay-redist"""; Flags: runhidden waituntilterminated; Components: airplay\bonjour; StatusMsg: "Installing Apple Bonjour Service for AirPlay discovery..."
Filename: "powershell.exe"; Parameters: "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File ""{tmp}\obs-airplay-redist\setup-prerequisites.ps1"" -Action InstallAppleDrivers -RedistDir ""{tmp}\obs-airplay-redist"" -ObsDir ""{app}"" -PluginBinDir ""{app}\obs-plugins\64bit"""; Flags: runhidden waituntilterminated; Components: airplay\apple_drivers; StatusMsg: "Installing Apple Mobile Device & USB Reverse Tethering drivers..."
Filename: "powershell.exe"; Parameters: "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File ""{tmp}\obs-airplay-redist\setup-prerequisites.ps1"" -Action ConfigureFirewall -ObsDir ""{app}"""; Flags: runhidden waituntilterminated; Components: airplay\firewall; StatusMsg: "Configuring Windows Defender Firewall rules for AirPlay & mDNS..."
Filename: "{app}\bin\64bit\obs64.exe"; Description: "Launch OBS Studio now"; Flags: postinstall nowait skipifsilent unchecked; Check: IsObsExePresent

[Code]
const
  TH32CS_SNAPPROCESS = $00000002;

type
  TProcessEntry32 = record
    dwSize: DWORD;
    cntUsage: DWORD;
    th32ProcessID: DWORD;
    th32DefaultHeapID: DWORD;
    th32ModuleID: DWORD;
    cntThreads: DWORD;
    th32ParentProcessID: DWORD;
    pcPriClassBase: Longint;
    dwFlags: DWORD;
    szExeFile: array[0..259] of WideChar;
  end;

function CreateToolhelp32Snapshot(dwFlags, th32ProcessID: DWORD): THandle;
  external 'CreateToolhelp32Snapshot@kernel32.dll stdcall';
function Process32First(hSnapshot: THandle; var lppe: TProcessEntry32): BOOL;
  external 'Process32FirstW@kernel32.dll stdcall';
function Process32Next(hSnapshot: THandle; var lppe: TProcessEntry32): BOOL;
  external 'Process32NextW@kernel32.dll stdcall';
function CloseHandle(hObject: THandle): BOOL;
  external 'CloseHandle@kernel32.dll stdcall';

function IsProcessRunning(const FileName: String): Boolean;
var
  hSnap: THandle;
  pe: TProcessEntry32;
  exeName: String;
  i: Integer;
begin
  Result := False;
  hSnap := CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if hSnap <> THandle(-1) then
  begin
    pe.dwSize := SizeOf(pe);
    if Process32First(hSnap, pe) then
    begin
      repeat
        exeName := '';
        for i := 0 to 259 do
        begin
          if pe.szExeFile[i] = #0 then Break;
          exeName := exeName + pe.szExeFile[i];
        end;
        if CompareText(exeName, FileName) = 0 then
        begin
          Result := True;
          Break;
        end;
      until not Process32Next(hSnap, pe);
    end;
    CloseHandle(hSnap);
  end;
end;

function CheckProcessesRunning(const ActionPrompt: String): Boolean;
begin
  Result := True;
  while IsProcessRunning('obs64.exe') or IsProcessRunning('obs32.exe') or IsProcessRunning('scrcpy.exe') do
  begin
    if MsgBox(
      'OBS Studio or scrcpy is currently running.' + #13#10 + #13#10 +
      'Please save your work and close OBS Studio / scrcpy before ' + ActionPrompt + '.' + #13#10 + #13#10 +
      'Click Retry once closed, or Cancel to abort.',
      mbError, MB_RETRYCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

function GetOBSInstallDir(Param: String): String;
var
  ObsDir: String;
begin
  Result := '';

  // 1. Check 64-bit Registry HKLM
  if RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\OBS Studio', '', ObsDir) then
  begin
    if (ObsDir <> '') and DirExists(ObsDir) then
    begin
      Result := ObsDir;
      Exit;
    end;
  end;

  // 2. Check HKCU Registry
  if RegQueryStringValue(HKEY_CURRENT_USER, 'SOFTWARE\OBS Studio', '', ObsDir) then
  begin
    if (ObsDir <> '') and DirExists(ObsDir) then
    begin
      Result := ObsDir;
      Exit;
    end;
  end;

  // 3. Check 32-bit Registry View HKLM
  if RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\WOW6432Node\OBS Studio', '', ObsDir) then
  begin
    if (ObsDir <> '') and DirExists(ObsDir) then
    begin
      Result := ObsDir;
      Exit;
    end;
  end;

  // 4. Default fallback to standard Program Files
  Result := ExpandConstant('{autopf}\obs-studio');
end;

function GetScrcpyInstallDir(Param: String): String;
begin
  Result := ExpandConstant('{commonpf}\scrcpy\4.1');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  ObsExePath: String;
  PluginDir: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    ObsExePath := ExpandConstant('{app}\bin\64bit\obs64.exe');
    PluginDir := ExpandConstant('{app}\obs-plugins\64bit');
    if not FileExists(ObsExePath) and not DirExists(PluginDir) then
    begin
      if MsgBox(
        'Warning: The chosen directory does not appear to be an OBS Studio installation folder.' + #13#10 +
        '("' + ObsExePath + '" was not found).' + #13#10 + #13#10 +
        'Are you sure you want to install the plugin into:' + #13#10 +
        ExpandConstant('{app}') + '?',
        mbConfirmation, MB_YESNO) <> IDYES then
      begin
        Result := False;
      end;
    end;
  end;
end;

procedure StopTetherHelper(DeleteTask: Boolean);
var
  ResultCode: Integer;
  HelperExe: String;
begin
  Exec(ExpandConstant('{sys}\schtasks.exe'), '/end /tn "OBS-AirPlay-Tether-Helper"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM obs-airplay-tether-helper.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  if DeleteTask then
  begin
    HelperExe := ExpandConstant('{app}\obs-plugins\64bit\obs-airplay-tether-helper.exe');
    if FileExists(HelperExe) then
    begin
      Exec(HelperExe, '--stop', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    end;
    Exec(ExpandConstant('{sys}\schtasks.exe'), '/delete /tn "OBS-AirPlay-Tether-Helper" /f', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec('powershell.exe', '-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -Command "Remove-NetFirewallRule -Group ''OBS scrcpy & AirPlay Source'' -ErrorAction SilentlyContinue"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  StopTetherHelper(False);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    StopTetherHelper(True);
  end;
end;

function InitializeSetup(): Boolean;
begin
  Result := CheckProcessesRunning('continuing with the installation');
end;

function InitializeUninstall(): Boolean;
begin
  Result := CheckProcessesRunning('uninstalling this plugin');
end;

function IsObsExePresent(): Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\bin\64bit\obs64.exe'));
end;
