; Celadon 的 Windows 安装包（Inno Setup 6.7）。
; 在服务器上：scripts/windows-wine.sh installer（先 msbuild 编 Release x64，再用 Wine 里的 ISCC 编本文件）；
; 在 Windows 上：先按 docs/BUILDING.md 编出 Release x64，再用 Inno Setup 6 打开本文件编译，或
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DAppVersion=0.1.8.6 installer\Celadon.iss
; 版本号由 /DAppVersion=… 传入（scripts/windows-wine.sh 从 Celadon.jucer 读），产物在 installer\Output\。
;
; 装什么：
;   - VST3：整个 Celadon.vst3 文件夹 → C:\Program Files\Common Files\VST3\（各宿主默认扫描的位置，要管理员权限）；
;   - 独立运行版（可选，默认装）：Celadon.exe → C:\Program Files\Celadon\，开始菜单一个快捷方式。
; 卸载：「设置 → 应用」里的 Celadon，删掉上面两处。用户设置（%APPDATA%\Celadon）不删，重装后界面语言、大小照旧。
; 没有代码签名：第一次运行安装包时 SmartScreen 会提示「未知发布者」，点「更多信息 → 仍要运行」。

#ifndef AppVersion
  #error 用 /DAppVersion=x.y.z.w 传入版本号（scripts/windows-wine.sh 从 Celadon.jucer 读）
#endif

#define BuildDir "..\Builds\VisualStudio2022\x64\Release"

[Setup]
; AppId 固定不变：升级安装与卸载都靠它认出同一个程序。
AppId={{C3C8CAF2-FB70-4E1E-BFEC-28377E20F5FC}
AppName=Celadon
AppVersion={#AppVersion}
AppVerName=Celadon {#AppVersion}
AppPublisher=Max Song
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\Celadon
DefaultGroupName=Celadon
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=Output
OutputBaseFilename=Celadon-{#AppVersion}-Windows-x64-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Celadon {#AppVersion}
SetupLogging=yes

[Languages]
; 按 Windows 的界面语言自动选（中文系统出中文向导）。简体中文文件不随 Inno Setup 发行，取自其官方仓库
; issrc 标签 is-6_7_3 的 Files/Languages/Unofficial/ChineseSimplified.isl，原样未改（见 Languages/README.md）。
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "Languages\ChineseSimplified.isl"

[CustomMessages]
english.TypeFull=VST3 plug-in and standalone app
english.TypeVst3=VST3 plug-in only
english.TypeCustom=Custom
english.ComponentVst3=VST3 plug-in (Common Files\VST3)
english.ComponentStandalone=Standalone app (runs without a DAW)
chinesesimplified.TypeFull=VST3 插件与独立运行版
chinesesimplified.TypeVst3=只装 VST3 插件
chinesesimplified.TypeCustom=自定义
chinesesimplified.ComponentVst3=VST3 插件（Common Files\VST3）
chinesesimplified.ComponentStandalone=独立运行版（不开宿主也能用）

[Types]
Name: "full"; Description: "{cm:TypeFull}"
Name: "vst3"; Description: "{cm:TypeVst3}"
Name: "custom"; Description: "{cm:TypeCustom}"; Flags: iscustom

[Components]
Name: "vst3"; Description: "{cm:ComponentVst3}"; Types: full vst3 custom; Flags: fixed
Name: "standalone"; Description: "{cm:ComponentStandalone}"; Types: full

[InstallDelete]
; 先整个删掉旧的 Celadon.vst3 再装：旧版本多出来的文件不会留在包里。
Type: filesandordirs; Name: "{commoncf64}\VST3\Celadon.vst3"; Components: vst3
; 上次装了独立版、这次没选：Inno 不会自己删取消勾选的组件，旧的 Celadon.exe 与开始菜单快捷方式在这里清掉。
Type: files; Name: "{app}\Celadon.exe"; Components: not standalone
Type: files; Name: "{autoprograms}\Celadon.lnk"; Components: not standalone

[Files]
Source: "{#BuildDir}\VST3\Celadon.vst3\*"; DestDir: "{commoncf64}\VST3\Celadon.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\Standalone Plugin\Celadon.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Celadon"; Filename: "{app}\Celadon.exe"; Components: standalone

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Celadon.vst3"
