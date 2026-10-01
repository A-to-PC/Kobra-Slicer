; Kobra Slicer -- Inno Setup installer script.
; Switched from CPack/NSIS 01/10/2026 -- NSIS itself wasn't installable on the build machine
; (download blocked by firewall, and the fallback winget package failed its own hash check),
; while Inno Setup was already installed and trusted. The underlying app build (CMake/CPack)
; is unchanged; this script packages the same `cmake --install` output CPack would have used.
;
; Build the staging tree first:
;   cmake --install build --config Release --prefix build/install_staging
; Then compile this script (ISCC.exe KobraSlicer.iss) to produce the installer.

#define MyAppName "Kobra Slicer"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "A to PC"
#define MyAppURL "https://github.com/A-to-PC/Kobra-Slicer"
#define MyAppExeName "kobra-slicer.exe"
#define StagingDir "build\install_staging"

[Setup]
AppId={{B6F2E6C4-6E0E-4C6A-9B1A-1A2B3C4D5E6F}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
; Explicit per-machine constants, not {autopf}/{autodesktop}/{group} -- those can split an
; install across per-user and per-machine depending on how Inno Setup detects the run context,
; even when run as admin. Confirmed real issue on this fork's other installers.
DefaultDirName={commonpf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=build\installer_output
OutputBaseFilename=KobraSlicer_Windows_Installer_V{#MyAppVersion}
SetupIconFile=resources\images\KobraSlicer.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; Everything cmake --install produced, except the stray include/ and lib/ folders -- those
; are a third-party dependency's own dev headers/cmake-config files that got swept into the
; install tree by its own install() rules, not anything an end user needs.
Source: "{#StagingDir}\*"; DestDir: "{app}"; Excludes: "include\*,lib\*"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{commondesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
