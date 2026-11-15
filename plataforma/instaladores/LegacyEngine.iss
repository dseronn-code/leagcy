; Instalador do Legacy Engine (editor + cenas de exemplo). iscc /DOrigem=<pasta> /DVersao=x.y.z
#ifndef Versao
  #define Versao "0.1.0"
#endif
[Setup]
AppId={{1A2B3C4D-5E6F-4071-8293-A4B5C6D7E8F9}
AppName=Legacy Engine
AppVersion={#Versao}
AppPublisher=Legacy
AppPublisherURL=https://legacyengine.com.br
DefaultDirName={autopf}\Legacy\Legacy Engine
DefaultGroupName=Legacy
UninstallDisplayName=Legacy Engine
OutputBaseFilename=LegacyEngine-Setup-{#Versao}
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
[Languages]
Name: "pt"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"
[Files]
Source: "{#Origem}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
[Icons]
Name: "{group}\Legacy Engine"; Filename: "{app}\legacy.exe"; Parameters: "--editor --scene cenas\vale\vale.json"; WorkingDir: "{app}"
Name: "{autodesktop}\Legacy Engine"; Filename: "{app}\legacy.exe"; Parameters: "--editor --scene cenas\vale\vale.json"; WorkingDir: "{app}"; Tasks: atalho
[Tasks]
Name: "atalho"; Description: "Criar atalho na área de trabalho"
[Run]
Filename: "{app}\legacy.exe"; Parameters: "--editor --scene cenas\vale\vale.json"; WorkingDir: "{app}"; Description: "Abrir o Legacy Engine"; Flags: nowait postinstall skipifsilent
[UninstallDelete]
Type: filesandordirs; Name: "{app}\texcache"
