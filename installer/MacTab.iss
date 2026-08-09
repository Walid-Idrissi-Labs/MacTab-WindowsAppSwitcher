; ============================================================================
; MacTab installer (Inno Setup 6.3+ — uses x64compatible and modern wizard)
;
; Build:
;   ISCC.exe /DMyAppVersion=1.2.3 /DSourceDir=..\build\bin installer\MacTab.iss
;
; Defaults below let you compile it locally with no defines for a smoke test.
;
; DESIGN DECISIONS (do not change casually):
;  * AppId is FROZEN. Changing it forks the upgrade lineage: existing installs
;    would no longer be upgraded in place and Add/Remove would show two entries.
;  * Per-user by default (PrivilegesRequired=lowest): installs to
;    %LOCALAPPDATA%\Programs\MacTab, no UAC prompt, no signing needed.
;    `MacTab-Setup.exe /ALLUSERS` performs a per-machine install today
;    (PrivilegesRequiredOverridesAllowed=commandline); when uiAccess="true"
;    lands (signed exe, must live under Program Files), flip the default by
;    changing PrivilegesRequired to admin — everything else already uses the
;    {auto*} constants, so nothing else in this script changes.
;  * The app-side contract this installer relies on (see ShutdownRunningInstance
;    and the CloseApplications directive):
;      1. The hidden window class name below matches the app.
;      2. WM_CLOSE on the hidden window => clean shutdown (unhook, save, exit).
;      3. WM_QUERYENDSESSION with lParam=ENDSESSION_CLOSEAPP => return TRUE;
;         WM_ENDSESSION (wParam=TRUE) => exit. Restart Manager uses these.
;      4. The Run-key written here is the single source of truth for autostart;
;         the app's settings UI must read/toggle the key itself, not a copy.
; ============================================================================

#define MyAppName       "MacTab"
#define MyAppExeName    "MacTab.exe"
#define MyAppPublisher  "Walid Idrissi Labs"
#define MyAppURL        "https://github.com/Walid-Idrissi-Labs/WindowsAppSwitcher"
; FROZEN — never regenerate:
#define MyAppId         "{E2C26C45-D5E7-4EFD-A956-4168F7C3E0D6}"
; Must match kHostWindowClass in src/app.h exactly. If these drift, the graceful
; shutdown below silently finds nothing and every upgrade falls back to Restart
; Manager force-closing the app.
#define MyWindowClass   "MacTabHostWindow"

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
; Numeric-only version for VERSIONINFO (no -beta suffixes). CI passes both.
#ifndef MyAppVersionNumeric
  #define MyAppVersionNumeric MyAppVersion
#endif
#ifndef SourceDir
  #define SourceDir "..\build\bin"
#endif

[Setup]
; The extra {} is Inno escaping: AppId must literally start with one brace.
AppId={{#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
VersionInfoVersion={#MyAppVersionNumeric}

; --- install mode: per-user default, per-machine via /ALLUSERS -------------
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
; {autopf} => %LOCALAPPDATA%\Programs (per-user) or %ProgramFiles% (admin).
DefaultDirName={autopf}\{#MyAppName}
DisableProgramGroupPage=yes
DisableDirPage=auto

; --- platform ---------------------------------------------------------------
; x64compatible = x64 plus ARM64 devices running the x64 build under emulation.
; Requires Inno Setup >= 6.3 (older syntax was "x64").
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 1809; raise/lower to match the oldest build you actually test on.
MinVersion=10.0.17763

; --- upgrade over a running instance ---------------------------------------
; Restart Manager: detects processes holding files we're about to replace and
; closes them via WM_QUERYENDSESSION/ENDSESSION_CLOSEAPP. This is the safety
; net; the primary path is our own graceful WM_CLOSE in PrepareToInstall.
; We relaunch explicitly in [Run], so don't let RM also restart it.
CloseApplications=yes
CloseApplicationsFilter=*.exe
RestartApplications=no

; --- output -----------------------------------------------------------------
OutputBaseFilename={#MyAppName}-Setup-{#MyAppVersion}
SetupIconFile=..\res\mactab.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
WizardStyle=modern
; LZMA2 is fine for AV heuristics; do NOT add exe packers (UPX etc.) on top.
Compression=lzma2/max
SolidCompression=yes

; --- future: code signing ---------------------------------------------------
; When you have a certificate, define a SignTool in the IDE/CI and uncomment:
; SignTool=mysigntool
; SignedUninstaller=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
; Checked by default. The app's own settings UI toggles the same Run key later.
Name: "autostart"; Description: "Start {#MyAppName} automatically when you sign in"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; HKA = HKCU in per-user mode, HKLM in per-machine mode. Both map onto the
; corresponding ...\CurrentVersion\Run key, so autostart works in either mode.
; uninsdeletevalue removes it on uninstall even if the task was toggled later.
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
  ValueType: string; ValueName: "{#MyAppName}"; \
  ValueData: """{app}\{#MyAppExeName}"""; \
  Flags: uninsdeletevalue; Tasks: autostart

[Run]
; Interactive installs: offer to launch (checked by default on the final page).
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName} now"; \
  Flags: nowait postinstall skipifsilent runasoriginaluser
; Silent upgrades (CI / winget / self-update): pass /STARTAPP=1 to relaunch.
Filename: "{app}\{#MyAppExeName}"; \
  Flags: nowait runasoriginaluser skipifnotsilent; Check: StartAppRequested

[Code]
const
  WM_CLOSE      = $0010;
  SYNCHRONIZE   = $00100000;
  WAIT_OBJECT_0 = 0;

function GetWindowThreadProcessId(Wnd: HWND; var Pid: DWORD): DWORD;
  external 'GetWindowThreadProcessId@user32.dll stdcall';
function OpenProcess(Access: DWORD; Inherit: LongBool; Pid: DWORD): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(H: THandle; Ms: DWORD): DWORD;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(H: THandle): LongBool;
  external 'CloseHandle@kernel32.dll stdcall';

function StartAppRequested(): Boolean;
begin
  Result := ExpandConstant('{param:STARTAPP|0}') = '1';
end;

{ Ask the running instance (found via its hidden window) to exit, then wait
  on its PROCESS handle — not just window destruction — so the exe's file
  lock is genuinely gone before files are replaced. Race-free order:
  open the process handle first, THEN post WM_CLOSE, THEN wait. }
function ShutdownRunningInstance(TimeoutMs: DWORD): Boolean;
var
  Wnd: HWND;
  Pid: DWORD;
  Proc: THandle;
  Waited: DWORD;
begin
  Result := True;
  Wnd := FindWindowByClassName('{#MyWindowClass}');
  if Wnd = 0 then
    Exit;

  Pid := 0;
  GetWindowThreadProcessId(Wnd, Pid);
  Proc := 0;
  if Pid <> 0 then
    Proc := OpenProcess(SYNCHRONIZE, False, Pid);

  PostMessage(Wnd, WM_CLOSE, 0, 0);

  if Proc <> 0 then
  begin
    Result := WaitForSingleObject(Proc, TimeoutMs) = WAIT_OBJECT_0;
    CloseHandle(Proc);
  end
  else
  begin
    { Could not open the process (odd, but possible across integrity levels):
      fall back to polling for the window, then padding for the file lock. }
    Waited := 0;
    while (FindWindowByClassName('{#MyWindowClass}') <> 0)
          and (Waited < TimeoutMs) do
    begin
      Sleep(200);
      Waited := Waited + 200;
    end;
    Result := FindWindowByClassName('{#MyWindowClass}') = 0;
    if Result then
      Sleep(500);
  end;
end;

{ Primary close path. If it fails we do NOT abort: CloseApplications (Restart
  Manager) and, as a last resort, Setup's in-use retry dialog still stand
  between us and a corrupt install. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not ShutdownRunningInstance(10000) then
    Log('MacTab did not exit within timeout; deferring to Restart Manager.');
end;

{ Future migration path: the first per-machine (/ALLUSERS) install silently
  removes an existing per-user install so the user doesn't end up with two
  copies and two ARP entries. No-op today while installs default to per-user. }
procedure RemovePerUserInstallIfMigrating();
var
  UninstStr: String;
  ResultCode: Integer;
begin
  if not IsAdminInstallMode then
    Exit;
  if RegQueryStringValue(HKEY_CURRENT_USER,
       'Software\Microsoft\Windows\CurrentVersion\Uninstall\{#MyAppId}_is1',
       'UninstallString', UninstStr) then
  begin
    UninstStr := RemoveQuotes(UninstStr);
    Log('Migrating: removing per-user install via ' + UninstStr);
    Exec(UninstStr, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '',
         SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    RemovePerUserInstallIfMigrating();
end;

{ --------------------------- uninstall ----------------------------------- }

{ User data policy: the uninstaller ASKS in interactive mode (default = keep),
  KEEPS data in silent mode unless /REMOVESETTINGS=1 was passed. It only ever
  touches the CURRENT user's %LOCALAPPDATA%\MacTab — other users' data on a
  per-machine install is deliberately left (their profiles may not even be
  loaded); the app removes or the user deletes it themselves. }
procedure MaybeRemoveUserData();
var
  DataDir: String;
  Remove: Boolean;
begin
  DataDir := ExpandConstant('{localappdata}\{#MyAppName}');
  if not DirExists(DataDir) then
    Exit;
  if UninstallSilent then
    Remove := ExpandConstant('{param:REMOVESETTINGS|0}') = '1'
  else
    Remove := MsgBox('Also remove your {#MyAppName} settings and icon cache?'
                     + #13#10#13#10 +
                     'Choose No to keep them for a future reinstall.',
                     mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
  if Remove then
    DelTree(DataDir, True, True, True);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    ShutdownRunningInstance(10000)
  else if CurUninstallStep = usPostUninstall then
    MaybeRemoveUserData();
end;
