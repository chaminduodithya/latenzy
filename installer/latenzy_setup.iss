; Inno Setup script for Latenzy  (https://jrsoftware.org/isinfo.php)
; Build the exe first (build.bat), then compile this script (ISCC installer\latenzy_setup.iss).
; Pass /DMyAppVersion=1.2.3 to override the version.

#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif
#define MyAppName "Latenzy"
#define MyAppExe  "latenzy.exe"

[Setup]
AppId={{6F3C2A18-9D47-4B5E-A1C2-7E8D90B4F1A3}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Latenzy
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputBaseFilename=Latenzy-Setup-{#MyAppVersion}
OutputDir=..\dist
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExe}
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "startup"; Description: "Start Latenzy with &Windows (in the tray)"; GroupDescription: "Options:"

[Files]
Source: "..\build\{#MyAppExe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Latenzy"; ValueData: """{app}\{#MyAppExe}"" --tray"; Flags: uninsdeletevalue; Tasks: startup

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM {#MyAppExe}"; Flags: runhidden; RunOnceId: "KillLatenzy"
