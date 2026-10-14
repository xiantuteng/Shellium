; Shellium 单用户 NSIS 安装器。所有状态都写入 HKCU 与 LocalAppData，普通用户无需 UAC。
Unicode true
RequestExecutionLevel user

!ifndef STAGING_DIR
  !error "必须通过 /DSTAGING_DIR=... 指定已完成 DLL/Qt 插件部署的目录"
!endif
!ifndef SHELLIUM_VERSION
  !define SHELLIUM_VERSION "0.0.0-dev"
!endif
!ifndef OUTPUT_FILE
  !define OUTPUT_FILE "Shellium-Setup-x64.exe"
!endif

Name "Shellium"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Shellium"
InstallDirRegKey HKCU "Software\Shellium" "InstallLocation"
ShowInstDetails show
ShowUninstDetails show

; SetShellVarContext 只能出现在 Section/Function 内。安装与卸载各自在初始化时固定为当前用户，
; 开始菜单快捷方式才会落在 $SMPROGRAMS 的用户目录而不是所有用户目录。
Function .onInit
  SetShellVarContext current
FunctionEnd

Function un.onInit
  SetShellVarContext current
FunctionEnd

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Shellium" SEC_MAIN
  SetOutPath "$INSTDIR"
  ; staging 已由部署步骤验证并包含 Qt 插件目录。/r 保持该目录拓扑，qwindows/qsqlite 才能加载。
  File /r "${STAGING_DIR}\*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\Shellium" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shellium" "DisplayName" "Shellium"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shellium" "DisplayVersion" "${SHELLIUM_VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shellium" "Publisher" "Shellium contributors"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shellium" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shellium" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shellium" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shellium" "NoRepair" 1
  CreateDirectory "$SMPROGRAMS\Shellium"
  CreateShortcut "$SMPROGRAMS\Shellium\Shellium.lnk" "$INSTDIR\Shellium.exe"
  CreateShortcut "$SMPROGRAMS\Shellium\卸载 Shellium.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Uninstall"
  ; 用户数据库、保险箱和日志位于 QStandardPaths::AppDataLocation，不在安装目录中。这里仅删除
  ; 程序与快捷方式，默认卸载和静默卸载都不会触碰用户数据或 ~/.ssh。
  Delete "$SMPROGRAMS\Shellium\Shellium.lnk"
  Delete "$SMPROGRAMS\Shellium\卸载 Shellium.lnk"
  RMDir "$SMPROGRAMS\Shellium"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shellium"
  DeleteRegKey HKCU "Software\Shellium"
SectionEnd
