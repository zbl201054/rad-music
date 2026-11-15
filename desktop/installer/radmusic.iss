
; Rad Music 桌面端安装包（Inno Setup 6）
;
; 为什么要有它：客户端改用「安装包更新」之后，更新动作 = 静默跑一遍这个安装包。
; zip 只能覆盖「包里有的文件」，而旧的 win64.zip 里没有 radmusic_desktop.exe，
; 桌面端永远升不了外壳自己；安装包是整体安装，外壳/服务端/web/插件一次换齐，
; 还顺便写好了开始菜单与卸载项。
;
; 编译（由 desktop/build_installer.sh 调用，不手工跑）：
;   RADMUSIC_VERSION=0.15.0 RADMUSIC_STAGE=<组装好的发布目录> RADMUSIC_OUT=<输出目录> \
;     ISCC.exe radmusic.iss
;
; 安装位置：默认 {autopf}\RadMusic —— 普通权限下就是 %LOCALAPPDATA%\Programs\RadMusic，
; 选了「为所有用户安装」时自动变成 Program Files。目录页始终显示，用户可以装到任意位置。
;
; 语言：简体中文 / English，安装向导开头可以选（静默安装不弹，按系统语言自动匹配）。

#define MyAppName "Rad Music"
#define MyAppVersion GetEnv("RADMUSIC_VERSION")
#define MyStage GetEnv("RADMUSIC_STAGE")
#define MyOut GetEnv("RADMUSIC_OUT")

[Setup]
; AppId 决定「升级 / 卸载」如何识别同一个产品：绝不能改
AppId={{8C1F4A62-2D75-4E19-9B47-5A3E0D6C7F21}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher=Rad Music
; {autopf}：普通权限 → %LOCALAPPDATA%\Programs，管理员 → Program Files
DefaultDirName={autopf}\RadMusic
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
; 目录页始终显示：安装位置必须可改（用户可能想装到别的盘）
DisableDirPage=no
; 按用户安装：免 UAC，且曲库（radmusic-data）就在安装目录旁边，必须可写
PrivilegesRequired=lowest
; 交互安装时允许用户自己选「为所有用户安装」（会弹 UAC）
PrivilegesRequiredOverridesAllowed=dialog
OutputDir={#MyOut}
OutputBaseFilename=rad-music-v{#MyAppVersion}-win64-desktop-setup
SetupIconFile=..\app.ico
UninstallDisplayIcon={app}\radmusic_desktop.exe
UninstallDisplayName={#MyAppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; 升级时自动用上次的安装目录（用户把程序挪走过也能对上）
UsePreviousAppDir=yes
; 静默更新时如果还有进程占着 exe/dll，让 Restart Manager 关掉它们（服务端已先退出，这里是兜底）
CloseApplications=yes
RestartApplications=no
; 多语言：每次都让用户选（静默安装被忽略，按系统语言自动匹配）
ShowLanguageDialog=yes
LanguageDetectionMethod=uilanguage
; 卸载时保留用户数据（除非用户明确选删除，见 [Code]）

[Languages]
; 顺序有意义：静默安装 / 无法判断系统语言时用第一条
Name: "zh"; MessagesFile: "ChineseSimplified.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
zh.CreateDesktopIcon=创建桌面快捷方式(&D)
en.CreateDesktopIcon=Create a &desktop shortcut
zh.AdditionalIcons=附加任务：
en.AdditionalIcons=Additional icons:
zh.LaunchApp=启动 {#MyAppName}
en.LaunchApp=Launch {#MyAppName}
zh.UninstallIcon=卸载 {#MyAppName}
en.UninstallIcon=Uninstall {#MyAppName}
zh.KeepDataPrompt=是否同时删除曲库数据？%n%1%n%n选「否」则保留（以后重装还能接着用）。
en.KeepDataPrompt=Also delete the music library?%n%1%n%nChoose No to keep it for a future reinstall.
zh.LibraryNote=曲库（radmusic-data）会建在安装目录旁边，之后可在「设置 → 数据」里迁移。
en.LibraryNote=The library folder (radmusic-data) is created next to the app; you can move it later in Settings > Data.

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; 整个发布目录（radmusic_desktop.exe / radmusic_server.exe / WebView2Loader.dll / web / plugins / Readme）
; Excludes 是保险：本地手工打包时 stage 里可能混进曲库、更新包缓存或日志
Source: "{#MyStage}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; \
    Excludes: "radmusic-data\*,updates\*,*.bak,*.log,__pycache__\*,*.pyc"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\radmusic_desktop.exe"; WorkingDir: "{app}"
Name: "{group}\{cm:UninstallIcon}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\radmusic_desktop.exe"; WorkingDir: "{app}"; Tasks: desktopicon

; 静默更新（/VERYSILENT）时这条也会执行 —— 客户端更新完必须把程序重新拉起来。
; 不要加 skipifsilent：那会让「点安装更新」之后程序直接消失。
[Run]
Filename: "{app}\radmusic_desktop.exe"; Description: "{cm:LaunchApp}"; WorkingDir: "{app}"; \
    Flags: nowait postinstall runasoriginaluser

[UninstallDelete]
; 更新包缓存属于程序产物，卸载时顺手清掉；曲库不动（见下方提示）
Type: filesandordirs; Name: "{app}\updates"

[Code]
// 卸载时问一句要不要连曲库一起删：默认保留（重装还能接着用）
// 提示文字走 {cm:...}，中文 / 英文各一份，跟着安装语言走。
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    DataDir := ExpandConstant('{app}\radmusic-data');
    if DirExists(DataDir) then
    begin
      if MsgBox(ExpandConstant('{cm:KeepDataPrompt,' + DataDir + '}'),
                mbConfirmation, MB_YESNO) = IDYES then
        DelTree(DataDir, True, True, True);
    end;
  end;
end;
