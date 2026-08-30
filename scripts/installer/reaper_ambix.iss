; ============================================================================
;  reaper_ambix Windows installer (Inno Setup 6).
;
;  This script is invoked by scripts\build_win.bat which passes:
;     /DReaperAmbixVersion=X.Y.Z
;     /DReaperAmbixStageDir=path\to\staged\plugin\folder
;     /DReaperAmbixOutputDir=path\to\put\setup.exe
;     /DReaperAmbixArchTag=win64|winarm64           (output filename suffix)
;     /DReaperAmbixArchAllowed=x64compatible|arm64  (Inno Setup architecture)
;  so this file does not need to be edited per-release.
; ============================================================================

#ifndef ReaperAmbixVersion
  #error You must invoke ISCC with /DReaperAmbixVersion=X.Y.Z (use scripts\build_win.bat)
#endif
#ifndef ReaperAmbixStageDir
  #error You must invoke ISCC with /DReaperAmbixStageDir=...
#endif
#ifndef ReaperAmbixOutputDir
  #error You must invoke ISCC with /DReaperAmbixOutputDir=...
#endif
; Architecture knobs default to x64 so a bare ISCC invocation still works.
#ifndef ReaperAmbixArchTag
  #define ReaperAmbixArchTag "win64"
#endif
#ifndef ReaperAmbixArchAllowed
  #define ReaperAmbixArchAllowed "x64compatible"
#endif

[Setup]
AppId={{E3C0D8F4-7A88-4E1F-9E5B-2C3F1A9D6B2E}
AppName=reaper_ambix
AppVersion={#ReaperAmbixVersion}
AppPublisher=Matthias Kronlachner
AppPublisherURL=https://www.matthiaskronlachner.com
AppSupportURL=https://www.matthiaskronlachner.com
DefaultDirName={userappdata}\REAPER\UserPlugins
DefaultGroupName=reaper_ambix
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir={#ReaperAmbixOutputDir}
OutputBaseFilename=reaper_ambix_v{#ReaperAmbixVersion}_{#ReaperAmbixArchTag}_setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed={#ReaperAmbixArchAllowed}
ArchitecturesInstallIn64BitMode={#ReaperAmbixArchAllowed}
WizardStyle=modern
UninstallDisplayName=reaper_ambix {#ReaperAmbixVersion}
LicenseFile={#SourcePath}\..\..\COPYING

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; Plugin DLL goes directly into REAPER\UserPlugins. Statically linked
; (libambix + WavPack + WDL) — no runtime deps to bundle alongside it.
Source: "{#ReaperAmbixStageDir}\reaper_ambix.dll"; \
    DestDir: "{app}"; Flags: ignoreversion

[Icons]

[Run]

[UninstallDelete]
; Clean up legacy bundled-libs folder from older installs (pre-static-link
; releases shipped libsndfile + deps under this subdir).
Type: filesandordirs; Name: "{app}\reaper_ambix-libs"

[Code]
function NeedsAddPath(Param: string): boolean;
begin
  Result := False;
end;
