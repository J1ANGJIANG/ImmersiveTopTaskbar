; ImmersiveTopTaskbar installer script (Inno Setup 6)
; Generates a single setup.exe with:
;   - default install dir  D:\ImmersiveTopTaskbar  (not C:)
;   - optional "run on startup" checkbox (default checked)
;   - Start Menu / Desktop shortcuts + uninstaller

#define MyAppName "ImmersiveTopTaskbar"
#define MyAppVersion "1.1.0"
#define MyAppPublisher "ImmersiveTopTaskbar"
#define MyAppExeName "ImmersiveTopTaskbar.exe"

[Setup]
AppId={{B3A7C9E1-4D6F-4F2A-9C0D-1E8A5B7F2A31}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName=D:\ImmersiveTopTaskbar
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; The install dir must not be forced to C:. Use a fixed default and let the
; user change it; silently fall back to {autopf} only if D:\ is unavailable.
OutputDir=dist
OutputBaseFilename=ImmersiveTopTaskbar-Setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; No admin rights needed: installs per-user into the chosen folder.
PrivilegesRequired=lowest
; Do not let the app run before install finishes.
UninstallDisplayIcon={app}\{#MyAppExeName}
; Keep the uninstaller stable.
SetupLogging=yes
; 安装向导左上角图标 + exe 卸载项图标共用 app.ico。
SetupIconFile=app.ico

[Languages]
; 只保留简体中文，安装向导对所有用户默认显示中文。
Name: "chinesesimplified"; MessagesFile: "installer\Languages\ChineseSimplified.isl"

[Tasks]
Name: "autostart"; Description: "开机自动启动 {#MyAppName}"; GroupDescription: "附加选项:"; Flags: checkedonce dontinheritcheck

[Files]
; The program body (must exist from build.cmd before compiling this script).
Source: "build\ImmersiveTopTaskbar.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Flags: createonlyiffileexists
Name: "{autostartup}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: autostart

[Run]
; 安装完成后让用户选择是否立即运行（托盘工具，静默安装时跳过）。
Filename: "{app}\{#MyAppExeName}"; Description: "立即运行 {#MyAppName}"; Flags: nowait postinstall skipifsilent
