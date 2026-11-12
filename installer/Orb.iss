#define AppName "Orb!"
#define AppVersion "0.2.0"
#define AppPublisher "Orb Technologies"

[Setup]
AppId={{A4B7C310-5DE1-4D38-9A6C-6E8D9D33E8C1}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\Programs\Orb
DefaultGroupName=Orb!
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
WizardStyle=modern
OutputDir=..\build\installer
OutputBaseFilename=Orb-Setup-{#AppVersion}
UninstallDisplayIcon={app}\bin\orb.exe
SetupLogging=yes
Compression=lzma2
SolidCompression=yes

[Types]
Name: "compact"; Description: "Compiler only"
Name: "full"; Description: "Full installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "compiler"; Description: "Orb! compiler + embedded standard modules + WebAssembly runtime"; Types: compact full custom; Flags: fixed
Name: "vscode"; Description: "Visual Studio Code language extension"; Types: full
Name: "examples"; Description: "Example projects"; Types: full
Name: "docs"; Description: "Offline documentation"; Types: full
Name: "stdlib"; Description: "Standard-library source files (for contributors)"; Types: full
Name: "websrc"; Description: "Browser host source (for Web runtime contributors)"; Types: full

[Tasks]
Name: "addtopath"; Description: "Add Orb! to the current user's PATH"; GroupDescription: "Optional integrations:"; Flags: checkedonce

[Files]
Source: "..\build\orb.exe"; DestDir: "{app}\bin"; Flags: ignoreversion; Components: compiler
Source: "..\examples\*"; DestDir: "{app}\examples"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: examples
Source: "..\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: docs
Source: "..\std\*"; DestDir: "{app}\std"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: stdlib
Source: "..\web\orb.js"; DestDir: "{app}\web"; Flags: ignoreversion; Components: websrc
Source: "..\build\dist\orb-lang-0.1.0.vsix"; DestDir: "{app}\extensions"; Flags: ignoreversion; Components: vscode

[Icons]
Name: "{group}\Orb! Documentation"; Filename: "{app}\docs\DISTRIBUTION.md"; Components: docs

[Code]
function HasPathEntry(const ExistingPath, Dir: string): Boolean;
var
  Work, Part: string;
  P: Integer;
begin
  Result := False;
  Work := ExistingPath;
  while Work <> '' do begin
    P := Pos(';', Work);
    if P = 0 then begin
      Part := Work;
      Work := '';
    end else begin
      Part := Copy(Work, 1, P - 1);
      Delete(Work, 1, P);
    end;
    if CompareText(Trim(Part), Dir) = 0 then begin
      Result := True;
      Exit;
    end;
  end;
end;

procedure AddOrbToPath;
var
  ExistingPath, Dir: string;
begin
  Dir := ExpandConstant('{app}\bin');
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', ExistingPath) then
    ExistingPath := '';
  if not HasPathEntry(ExistingPath, Dir) then begin
    if ExistingPath <> '' then ExistingPath := ExistingPath + ';';
    ExistingPath := ExistingPath + Dir;
    RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', ExistingPath);
  end;
end;

procedure RemoveOrbFromPath;
var
  ExistingPath, Dir, Work, Part, NewPath: string;
  P: Integer;
begin
  Dir := ExpandConstant('{app}\bin');
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', ExistingPath) then Exit;
  Work := ExistingPath;
  NewPath := '';
  while Work <> '' do begin
    P := Pos(';', Work);
    if P = 0 then begin
      Part := Work;
      Work := '';
    end else begin
      Part := Copy(Work, 1, P - 1);
      Delete(Work, 1, P);
    end;
    if (Trim(Part) <> '') and (CompareText(Trim(Part), Dir) <> 0) then begin
      if NewPath <> '' then NewPath := NewPath + ';';
      NewPath := NewPath + Part;
    end;
  end;
  RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', NewPath);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('addtopath') then
    AddOrbToPath;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RemoveOrbFromPath;
end;
