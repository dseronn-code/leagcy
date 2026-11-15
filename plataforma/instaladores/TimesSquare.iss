; Instalador da Times Square (Inno Setup). O GitHub Actions gera com: iscc /DOrigem=<pasta exportada> /DVersao=x.y.z
#ifndef Versao
  #define Versao "0.1.0"
#endif
[Setup]
AppId={{6E3B1F7A-1C2D-4E5F-9A0B-7C8D9E0F1A2B}
AppName=Times Square
AppVersion={#Versao}
AppPublisher=Legacy
AppPublisherURL=https://legacyengine.com.br
DefaultDirName={autopf}\Legacy\Times Square
DefaultGroupName=Legacy
UninstallDisplayName=Times Square
OutputBaseFilename=TimesSquare-Setup-{#Versao}
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
DisableProgramGroupPage=yes
; o motor grava o cache das texturas (texcache/) ao lado do jogo: a pasta precisa ser gravável pelo usuário
PrivilegesRequired=lowest
[Languages]
Name: "pt"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"
[Files]
Source: "{#Origem}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
[Icons]
Name: "{group}\Times Square"; Filename: "{app}\legacy.exe"; Parameters: "--scene cenas\times_square.json --fullscreen"; WorkingDir: "{app}"
Name: "{autodesktop}\Times Square"; Filename: "{app}\legacy.exe"; Parameters: "--scene cenas\times_square.json --fullscreen"; WorkingDir: "{app}"; Tasks: atalho
[Tasks]
Name: "atalho"; Description: "Criar atalho na área de trabalho"
[Run]
Filename: "{app}\legacy.exe"; Parameters: "--scene cenas\times_square.json --fullscreen"; WorkingDir: "{app}"; Description: "Jogar agora"; Flags: nowait postinstall skipifsilent
[UninstallDelete]
Type: filesandordirs; Name: "{app}\texcache"
