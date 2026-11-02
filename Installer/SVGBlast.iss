; Inno Setup 6 script for SVGBlast. Build with build-release.ps1, which passes the version
; read from the built exe; building it directly with ISCC.exe uses the default below.

#define AppName "SVGBlast"
#ifndef AppVersion
  #define AppVersion "1.2"
#endif
#define AppExe "SVGBlast.exe"
#define ProgId "SVGBlast.svg"
#define RepoUrl "https://github.com/Zayno/SVGBlast"

[Setup]
AppId={{D0EE956D-69A7-488F-877B-59B91DE17E03}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Wiaam Suleiman
AppPublisherURL={#RepoUrl}
AppSupportURL={#RepoUrl}/issues
AppUpdatesURL={#RepoUrl}/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
; Installs for the current user without admin rights by default; the user can pick "all users" instead
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
ChangesAssociations=yes
SetupIconFile=..\SVGBlastProj\MyIcon.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir=Output
OutputBaseFilename={#AppName}-{#AppVersion}-Setup

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\SVGBlastProj\x64\Release\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; HKA is HKCU for a current-user install and HKLM for an all-users install.

; File type that opens .svg files in SVGBlast
Root: HKA; Subkey: "Software\Classes\{#ProgId}"; ValueType: string; ValueName: ""; ValueData: "SVG Image"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\{#ProgId}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"
Root: HKA; Subkey: "Software\Classes\{#ProgId}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""

; Default handler for .svg. Windows only honours this while the user has not picked another app
; for .svg themselves; that choice can only be changed by the user (see [Run]).
Root: HKA; Subkey: "Software\Classes\.svg"; ValueType: string; ValueName: ""; ValueData: "{#ProgId}"; Flags: uninsdeletevalue
; Also lists SVGBlast under "Open with". Deliberately no Software\Classes\Applications\SVGBlast.exe key:
; Windows keys that by file name only, so it would take over (and on uninstall delete) the
; "Open with" choice of anyone already using the portable exe.
Root: HKA; Subkey: "Software\Classes\.svg\OpenWithProgids"; ValueType: string; ValueName: "{#ProgId}"; ValueData: ""; Flags: uninsdeletevalue

; Lists SVGBlast in Settings > Apps > Default apps
Root: HKA; Subkey: "Software\{#AppName}"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\{#AppName}\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "{#AppName}"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\{#AppName}\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "A fast, lightweight SVG viewer"
Root: HKA; Subkey: "Software\{#AppName}\Capabilities\FileAssociations"; ValueType: string; ValueName: ".svg"; ValueData: "{#ProgId}"
Root: HKA; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "{#AppName}"; ValueData: "Software\{#AppName}\Capabilities"; Flags: uninsdeletevalue

[Run]
; Windows 10/11 do not let installers change the user's default app, so offer its settings page instead
Filename: "ms-settings:defaultapps?registeredAppUser={#AppName}"; Description: "Make {#AppName} the default SVG viewer (opens Windows settings)"; Flags: postinstall shellexec skipifsilent; Check: not IsAdminInstallMode
Filename: "ms-settings:defaultapps?registeredAppMachine={#AppName}"; Description: "Make {#AppName} the default SVG viewer (opens Windows settings)"; Flags: postinstall shellexec skipifsilent; Check: IsAdminInstallMode
