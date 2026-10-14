; Shellium 单用户 NSIS 安装器。所有状态都写入 HKCU 与 LocalAppData，普通用户无需 UAC。
Unicode true
RequestExecutionLevel user

!ifndef STAGING_DIR
  !error "必须通过 /DSTAGING_DIR=... 指定已完成 DLL/Qt 插件部署的目录"
!endif
; 两个打包入口都从 CMakeLists 的 project() 读出纯数字 X.Y.Z，下面拼成 VIProductVersion 要求的四段式。
!ifndef SHELLIUM_VERSION
  !define SHELLIUM_VERSION "0.0.0"
!endif
!ifndef OUTPUT_FILE
  !define OUTPUT_FILE "Shellium-Setup-x64.exe"
!endif

; makensis 编译前会切换到脚本所在目录（/NOCD 才关闭），仓库内资源按相对本脚本的路径引用，
; 与调用者的工作目录无关。分隔符用 /：Linux/macOS 的 makensis 不把 \ 当路径分隔符。
!define SHELLIUM_ICON "Shellium.ico"
!define SHELLIUM_LICENSE "../../LICENSE"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shellium"

!include "MUI2.nsh"

Name "Shellium"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Shellium"
InstallDirRegKey HKCU "Software\Shellium" "InstallLocation"
ShowInstDetails show
ShowUninstDetails show

; 安装器自身的版本资源同样只用 ASCII，资源管理器属性页与 SmartScreen 显示的是这里的字符串。
VIProductVersion "${SHELLIUM_VERSION}.0"
VIAddVersionKey /LANG=0 "ProductName" "Shellium"
VIAddVersionKey /LANG=0 "FileDescription" "Shellium Setup"
VIAddVersionKey /LANG=0 "CompanyName" "Shellium contributors"
VIAddVersionKey /LANG=0 "LegalCopyright" "Copyright (C) 2026 Shellium contributors"
VIAddVersionKey /LANG=0 "ProductVersion" "${SHELLIUM_VERSION}"
VIAddVersionKey /LANG=0 "FileVersion" "${SHELLIUM_VERSION}"

!define MUI_ICON "${SHELLIUM_ICON}"
!define MUI_UNICON "${SHELLIUM_ICON}"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_SMALLDESC
!define MUI_FINISHPAGE_RUN "$INSTDIR\Shellium.exe"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SHELLIUM_LICENSE}"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; 第一个语言是兜底；系统界面语言为简体中文时 NSIS 自动选中文，不弹语言选择框。
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

LangString SEC_MAIN_NAME ${LANG_ENGLISH} "Shellium (required)"
LangString SEC_MAIN_NAME ${LANG_SIMPCHINESE} "Shellium（必需）"
LangString SEC_STARTMENU_NAME ${LANG_ENGLISH} "Start menu shortcuts"
LangString SEC_STARTMENU_NAME ${LANG_SIMPCHINESE} "开始菜单快捷方式"
LangString SEC_DESKTOP_NAME ${LANG_ENGLISH} "Desktop shortcut"
LangString SEC_DESKTOP_NAME ${LANG_SIMPCHINESE} "桌面快捷方式"
LangString DESC_MAIN ${LANG_ENGLISH} "Program files. Your hosts, vault and logs live in your user profile and are never touched."
LangString DESC_MAIN ${LANG_SIMPCHINESE} "程序文件。主机、保险箱与日志保存在用户目录，安装与卸载都不会改动。"
LangString DESC_STARTMENU ${LANG_ENGLISH} "Add Shellium and its uninstaller to the Start menu."
LangString DESC_STARTMENU ${LANG_SIMPCHINESE} "在开始菜单添加 Shellium 及卸载程序。"
LangString DESC_DESKTOP ${LANG_ENGLISH} "Put a Shellium shortcut on the desktop."
LangString DESC_DESKTOP ${LANG_SIMPCHINESE} "在桌面放置 Shellium 快捷方式。"
LangString UNINSTALL_LINK ${LANG_ENGLISH} "Uninstall Shellium"
LangString UNINSTALL_LINK ${LANG_SIMPCHINESE} "卸载 Shellium"

; SetShellVarContext 只能出现在 Section/Function 内。安装与卸载各自在初始化时固定为当前用户，
; 开始菜单与桌面快捷方式才会落在当前用户目录而不是所有用户目录。
Function .onInit
  SetShellVarContext current
FunctionEnd

Function un.onInit
  SetShellVarContext current
FunctionEnd

Section "$(SEC_MAIN_NAME)" SEC_MAIN
  SectionIn RO
  SetOutPath "$INSTDIR"
  ; staging 已由部署步骤验证并包含 Qt 插件目录。/r 保持该目录拓扑，qwindows/qsqlite 才能加载。
  File /r "${STAGING_DIR}\*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\Shellium" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "Shellium"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" '"$INSTDIR\Shellium.exe",0'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${SHELLIUM_VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "Shellium contributors"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Section "$(SEC_STARTMENU_NAME)" SEC_STARTMENU
  CreateDirectory "$SMPROGRAMS\Shellium"
  CreateShortcut "$SMPROGRAMS\Shellium\Shellium.lnk" "$INSTDIR\Shellium.exe" "" "$INSTDIR\Shellium.exe" 0
  CreateShortcut "$SMPROGRAMS\Shellium\$(UNINSTALL_LINK).lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "$(SEC_DESKTOP_NAME)" SEC_DESKTOP
  CreateShortcut "$DESKTOP\Shellium.lnk" "$INSTDIR\Shellium.exe" "" "$INSTDIR\Shellium.exe" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_MAIN} $(DESC_MAIN)
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_STARTMENU} $(DESC_STARTMENU)
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DESKTOP} $(DESC_DESKTOP)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  ; 用户数据库、保险箱和日志位于 QStandardPaths::AppDataLocation，不在安装目录中。这里仅删除
  ; 程序与快捷方式，默认卸载和静默卸载都不会触碰用户数据或 ~/.ssh。
  ; 卸载时的界面语言可能与安装时不同，两种语言的卸载快捷方式名都要删。
  Delete "$DESKTOP\Shellium.lnk"
  Delete "$SMPROGRAMS\Shellium\Shellium.lnk"
  Delete "$SMPROGRAMS\Shellium\Uninstall Shellium.lnk"
  Delete "$SMPROGRAMS\Shellium\卸载 Shellium.lnk"
  RMDir "$SMPROGRAMS\Shellium"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "Software\Shellium"
SectionEnd
