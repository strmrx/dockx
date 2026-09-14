; DockX -- Inno Setup installer script
; ---------------------------------------------------------------------------
; This builds the Windows .exe installer for DockX (the OBS Studio plugin).
;
; The version is passed IN from package.ps1 (/DMyAppVersion=x.y.z), which reads
; it from buildspec.json -- so the version lives in ONE place (buildspec.json)
; and is never hand-copied here. If you open this in the Inno Setup IDE directly
; without that define, it falls back to 0.0.0 so it still compiles for testing.
;
; To build: install Inno Setup 6 (free, https://jrsoftware.org/isdl.php), then run
;   powershell -ExecutionPolicy Bypass -File installer\package.ps1
; from the repo root. That produces both the .exe installer and the .zip in release\.
; ---------------------------------------------------------------------------

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#define MyAppName "DockX"
#define MyAppPublisher "StrmrX"
#define MyAppURL "https://strmrx.com/dockx"

[Setup]
; AppId uniquely identifies DockX for upgrades/uninstall. NEVER change it between
; versions, or Windows treats each release as a separate program.
AppId={{A7C3E1F2-9B4D-4E6A-8C1F-2D3E4F5A6B7C}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} OBS Studio plugin installer

; DockX installs INTO the existing OBS Studio folder. DefaultDirName is resolved
; at runtime from the OBS registry key (see GetOBSDir below); the user can still
; correct it on the folder page if OBS lives somewhere unusual.
DefaultDirName={code:GetOBSDir}
DisableDirPage=no
DisableProgramGroupPage=yes
UninstallDisplayName={#MyAppName} {#MyAppVersion}

; Writing into Program Files\obs-studio needs admin.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; If OBS is running, this mutex makes the installer ask the user to close it first,
; so we never try to overwrite a DLL OBS has loaded. (OBS Studio's core mutex.)
AppMutex=OBSStudioCore

OutputDir=..\release
OutputBaseFilename=DockX-{#MyAppVersion}-windows-x64-Installer
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

; Shown as a page before install: what DockX is + the SmartScreen reassurance.
InfoBeforeFile=welcome.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; The plugin DLL -> OBS's 64-bit plugin folder.
Source: "..\dist\dockx.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
; The (near-empty) locale file -> OBS's plugin data folder for DockX.
Source: "..\dist\locale\en-US.ini"; DestDir: "{app}\data\obs-plugins\dockx\locale"; Flags: ignoreversion

[Run]
; Offer to open the DockX page after install.
Filename: "{#MyAppURL}"; Description: "Visit strmrx.com/dockx for guides"; Flags: postinstall shellexec nowait skipifsilent unchecked

[UninstallDelete]
; Clean up the DockX data folder we created.
Type: filesandordirs; Name: "{app}\data\obs-plugins\dockx"

[Code]
{ Find the OBS Studio install folder from the registry the OBS installer writes.
  Falls back to the standard Program Files location if the key is missing. }
function GetOBSDir(Param: String): String;
var
  Path: String;
begin
  Path := '';
  if RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Path) and (Path <> '') then
    Result := Path
  else if RegQueryStringValue(HKLM32, 'SOFTWARE\OBS Studio', '', Path) and (Path <> '') then
    Result := Path
  else
    Result := ExpandConstant('{autopf}\obs-studio');
end;

{ Warn if the chosen folder does not look like an OBS install, but let the user
  proceed (some portable/custom setups are valid). }
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    if not DirExists(ExpandConstant('{app}\obs-plugins\64bit')) then
    begin
      if MsgBox('This does not look like an OBS Studio folder (no obs-plugins\64bit inside).'
        + #13#10 + #13#10 + 'Install here anyway?', mbConfirmation, MB_YESNO) = IDNO then
        Result := False;
    end;
  end;
end;
