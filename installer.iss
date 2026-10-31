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
Source: "release\RelWithDebInfo\streamtime\bin\64bit\*"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "release\RelWithDebInfo\streamtime\data\*"; DestDir: "{app}\data\obs-plugins\streamtime"; Flags: ignoreversion recursesubdirs createallsubdirs
