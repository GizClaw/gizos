!define GIZLOADER_HOOK_DIR "${__FILEDIR__}"

!macro GizloaderUserPath action
  InitPluginsDir
  File /oname=$PLUGINSDIR\gizloader-user-path.ps1 "${GIZLOADER_HOOK_DIR}\user-path.ps1"
  ExecWait '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -WindowStyle Hidden -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\gizloader-user-path.ps1" -Action ${action} -Directory "$INSTDIR"' $0
  ${If} $0 != 0
    DetailPrint "Gizloader PATH registration failed (exit $0)"
    SetErrorLevel 1
    Abort
  ${EndIf}
  System::Call 'user32::SendMessageTimeoutW(p 0xffff, i 0x001A, p 0, w "Environment", i 2, i 5000, *p .r0)'
!macroend

!macro NSIS_HOOK_PREINSTALL
  SetShellVarContext current
  StrCpy $INSTDIR "$LOCALAPPDATA\Programs\Gizloader"
  SetOutPath "$INSTDIR"
!macroend

!macro NSIS_HOOK_POSTINSTALL
  SetOutPath "$INSTDIR"
  File "${GIZLOADER_HOOK_DIR}\gizloader.cmd"
  !insertmacro GizloaderUserPath install
!macroend

!macro NSIS_HOOK_PREUNINSTALL
  !insertmacro GizloaderUserPath uninstall
  Delete "$INSTDIR\gizloader.cmd"
!macroend
