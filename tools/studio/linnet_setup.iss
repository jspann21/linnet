; Build inputs are supplied by media_distribution.py.
#ifndef PackageDir
  #error PackageDir is required
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif
#ifndef OutputName
  #error OutputName is required
#endif
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef AppVersionNumeric
  #error AppVersionNumeric is required
#endif
#ifndef AppUserModelID
  #error AppUserModelID is required
#endif

[Setup]
AppId=Linnet.MediaManager
AppName=Linnet
AppVersion={#AppVersion}
VersionInfoVersion={#AppVersionNumeric}
VersionInfoProductVersion={#AppVersionNumeric}
VersionInfoTextVersion={#AppVersion}
VersionInfoProductTextVersion={#AppVersion}
AppPublisher=Linnet contributors
AppPublisherURL=https://github.com/jspann21/linnet
DefaultDirName={localappdata}\Programs\Linnet
DefaultGroupName=Linnet
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile={#PackageDir}\_internal\assets\linnet.ico
WizardSmallImageFile={#PackageDir}\_internal\assets\linnet-icon.png
WizardImageFile={#PackageDir}\_internal\assets\linnet-setup.png
LicenseFile={#PackageDir}\LICENSE
UninstallDisplayIcon={app}\Linnet Media.exe,0
UninstallDisplayName=Linnet
; Kept in sync with the lifetime guard in media_app.py.
AppMutex=Local\Linnet.MediaManager.Running
SetupMutex=Local\Linnet.MediaManager.Setup
CloseApplications=no
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Linnet"; Filename: "{app}\Linnet Media.exe"; WorkingDir: "{app}"; IconFilename: "{app}\Linnet Media.exe"; IconIndex: 0; AppUserModelID: "{#AppUserModelID}"
Name: "{autodesktop}\Linnet"; Filename: "{app}\Linnet Media.exe"; WorkingDir: "{app}"; IconFilename: "{app}\Linnet Media.exe"; IconIndex: 0; AppUserModelID: "{#AppUserModelID}"; Tasks: desktopicon

[Run]
Filename: "{app}\Linnet Media.exe"; Description: "Open Linnet"; Flags: nowait postinstall skipifsilent

[Code]
var
  ChangeMutex: THandle;

function CreateMutex(Attributes: NativeUInt; InitialOwner: Boolean; Name: String): THandle;
  external 'CreateMutexW@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

procedure EndChanges;
begin
  if ChangeMutex <> 0 then begin
    CloseHandle(ChangeMutex);
    ChangeMutex := 0;
  end;
end;

function BeginChanges: String;
var
  AlreadyChanging: Boolean;
begin
  Result := '';
  if ChangeMutex <> 0 then Exit;
  ChangeMutex := CreateMutex(0, False, 'Local\Linnet.MediaManager.Changing');
  AlreadyChanging := DLLGetLastError = 183;
  if ChangeMutex = 0 then
    Result := 'Linnet setup could not protect the application files. Please try again.'
  else if AlreadyChanging then
    Result := 'Another Linnet installation or removal is in progress.';
  if Result = '' then
    if CheckForMutexes('Local\Linnet.MediaManager.Running') then
      Result := 'Close Linnet and wait for its operations to finish, then try again.';
  if Result <> '' then EndChanges;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := BeginChanges;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then EndChanges;
end;

procedure DeinitializeSetup;
begin
  EndChanges;
end;

function InitializeUninstall: Boolean;
var
  Problem: String;
begin
  Problem := BeginChanges;
  Result := Problem = '';
  if not Result then SuppressibleMsgBox(Problem, mbError, MB_OK, IDOK);
end;

procedure DeinitializeUninstall;
begin
  EndChanges;
end;
