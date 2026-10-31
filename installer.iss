[Setup]
AppName=StreamTime OBS Plugin
AppVersion=1.0
DefaultDirName={commonpf64}\obs-studio
DisableDirPage=no
DefaultGroupName=StreamTime
OutputDir=output
OutputBaseFilename=StreamTime-Setup
Compression=lzma
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UsePreviousAppDir=no

[Files]
[Files]
Source: "release\RelWithDebInfo\obs-plugins\*"; DestDir: "{app}\obs-plugins"; Flags: recursesubdirs ignoreversion
Source: "release\RelWithDebInfo\data\*"; DestDir: "{app}\data"; Flags: recursesubdirs ignoreversion
