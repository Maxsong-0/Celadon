; Celadon 的 Windows 安装包（Inno Setup 6.7）。
; 在服务器上：scripts/windows-wine.sh installer（先 msbuild 编 Release x64，再用 Wine 里的 ISCC 编本文件）；
; 在 Windows 上：先按 docs/BUILDING.md 编出 Release x64，再用 Inno Setup 6 打开本文件编译，或
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DAppVersion=<Celadon.jucer 里的 version> installer\Celadon.iss
; 版本号由 /DAppVersion=… 传入（scripts/windows-wine.sh 从 Celadon.jucer 读），产物在 installer\Output\。
;
; 装什么（组件页可多选，默认两项都勾上；不带 /TYPE、/COMPONENTS 的静默安装也是两项都装）：
;   - VST3：整个 Celadon.vst3 文件夹 → C:\Program Files\Common Files\VST3\（各宿主默认扫描的位置，要管理员权限）；
;   - 独立运行版：Celadon.exe → C:\Program Files\Celadon\，开始菜单一个快捷方式。
; 两项都能取消。一项都不选时向导会拦住（静默安装若用空的 /COMPONENTS 同样失败）。没有 AU（AU 只在 Mac 安装包里），也没有 VST2。
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
; 类型不是 custom 时，Inno 默认藏起组件勾选。这里始终显示，打开向导就能取消 VST3 或独立运行版。
AlwaysShowComponentsList=yes
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
english.TypeStandalone=Standalone app only
english.TypeCustom=Custom
english.ComponentVst3=VST3 plug-in (Common Files\VST3)
english.ComponentStandalone=Standalone app (runs without a DAW)
english.NeedOneComponent=Select at least one item: the VST3 plug-in, the standalone app, or both.
chinesesimplified.TypeFull=VST3 插件与独立运行版
chinesesimplified.TypeVst3=只装 VST3 插件
chinesesimplified.TypeStandalone=只装独立运行版
chinesesimplified.TypeCustom=自定义
chinesesimplified.ComponentVst3=VST3 插件（Common Files\VST3）
chinesesimplified.ComponentStandalone=独立运行版（不开宿主也能用）
chinesesimplified.NeedOneComponent=请至少选择一项：VST3 插件、独立运行版，或两项都要。

[Types]
; 第一项是默认类型：交互向导打开就是它，静默安装不带 /TYPE 时也是它。两项都包含在 full 里。
Name: "full"; Description: "{cm:TypeFull}"
Name: "vst3"; Description: "{cm:TypeVst3}"
Name: "standalone"; Description: "{cm:TypeStandalone}"
Name: "custom"; Description: "{cm:TypeCustom}"; Flags: iscustom

[Components]
; 不写 fixed：两项都能取消。用手改勾选时，Inno 把类型显示成 custom，但不会把剩下的勾选清掉。
; 下拉框里的「只装 VST3 / 只装独立运行版」，以及静默 /TYPE=vst3、/TYPE=standalone，仍按预设勾选。
Name: "vst3"; Description: "{cm:ComponentVst3}"; Types: full vst3 custom
Name: "standalone"; Description: "{cm:ComponentStandalone}"; Types: full standalone custom

[InstallDelete]
; 先整个删掉旧的 Celadon.vst3：这次勾了 VST3 就由下面的 [Files] 重新装上（旧版本多出来的文件不会留在包里）；
; 这次没勾则到此为止。取消勾选时 Inno 不会自己删掉上次的组件。
Type: filesandordirs; Name: "{commoncf64}\VST3\Celadon.vst3"
; 上次装了独立版、这次没选：旧的 Celadon.exe 与开始菜单快捷方式在这里清掉。
Type: files; Name: "{app}\Celadon.exe"; Components: not standalone
Type: files; Name: "{autoprograms}\Celadon.lnk"; Components: not standalone

[Files]
Source: "{#BuildDir}\VST3\Celadon.vst3\*"; DestDir: "{commoncf64}\VST3\Celadon.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\Standalone Plugin\Celadon.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Celadon"; Filename: "{app}\Celadon.exe"; Components: standalone

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Celadon.vst3"

[Code]
function AnyComponentSelected: Boolean;
begin
  Result := WizardIsComponentSelected('vst3') or WizardIsComponentSelected('standalone');
end;

// 组件页上两项都空着时不让走到下一步。
// 用 SuppressibleMsgBox：/VERYSILENT /SUPPRESSMSGBOXES 时不弹窗（无界面不会卡住），
// 静默安装停在这一页，Inno 记「Failed to proceed」后退出。交互安装则弹出说明。
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectComponents) and (not AnyComponentSelected) then
  begin
    SuppressibleMsgBox(CustomMessage('NeedOneComponent'), mbError, MB_OK, IDOK);
    Result := False;
  end;
end;

// 交互时若绕过了组件页，准备安装那一页仍会写出同一句说明。静默时这条对话框可被压掉。
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if AnyComponentSelected then
    Result := ''
  else
    Result := CustomMessage('NeedOneComponent');
end;
