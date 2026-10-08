; JeffPub Windows installer (NSIS). Built by CI from dist\JeffPub.
; Per-user install: no administrator rights needed.
Unicode true
ManifestDPIAware true
!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!define APP "JeffPub"
!define EXE "JeffPub.exe"
!define UNKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\JeffPub"
; JeffPub was called JeffPub 79 through version 0.5.0; setup replaces that install.
!define OLDKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\JeffPub79"
; Where JeffPub keeps its settings (QSettings: organization JeffOffice).
!define SETKEY "Software\JeffOffice\JeffPub"

Name "${APP}"
OutFile "JeffPub-Setup.exe"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\${APP}"
InstallDirRegKey HKCU "Software\JeffOffice" "JeffPubInstallDir"
BrandingText "${APP} ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "FileDescription" "${APP} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "JeffOffice LLC"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 JeffOffice LLC. Free software under the GNU GPL v3."

!define MUI_ICON "..\..\resources\app.ico"
!define MUI_UNICON "..\..\resources\app.ico"
!define MUI_ABORTWARNING
; Branded artwork (tools/make_installer_art.py), drawn at twice the size so it
; stays sharp on high-DPI screens.
!define MUI_WELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "header.bmp"
!define MUI_HEADERIMAGE_UNBITMAP "header.bmp"
!define MUI_WELCOMEPAGE_TITLE "Welcome to ${APP}"
!define MUI_WELCOMEPAGE_TEXT "This will install ${APP} ${VERSION}, a free, open-source desktop publishing program that opens .pub files.$\r$\n$\r$\n${APP} is an independent open-source project.$\r$\n$\r$\nNo administrator rights are needed."
!insertmacro MUI_PAGE_WELCOME
; The GNU GPL v3.0: Next stays disabled until the box is checked.
!define MUI_LICENSEPAGE_TEXT_TOP "${APP} is free software under the GNU General Public License v3.0."
!define MUI_LICENSEPAGE_TEXT_BOTTOM "Please read the license. To install ${APP}, you must accept its terms."
!define MUI_LICENSEPAGE_CHECKBOX
!define MUI_LICENSEPAGE_CHECKBOX_TEXT "I accept the terms of the GNU General Public License v3.0"
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE "${APP} is ready"
!define MUI_FINISHPAGE_TEXT "${APP} ${VERSION} is installed. You'll find it in the Start menu."
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP}"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; Files in use can't be replaced, so JeffPub must be closed first. Since
; 0.1.6 it holds a named mutex while it runs; any version keeps its .exe
; locked, which catches older ones too.
!macro WaitForAppToClose
  check_running:
    StrCpy $R9 0
    System::Call 'kernel32::OpenMutexW(i 0x00100000, i 0, w "JeffPubRunning") p .r1'
    IntCmp $1 0 no_mutex
      System::Call 'kernel32::CloseHandle(p r1)'
      StrCpy $R9 1
  no_mutex:
    System::Call 'kernel32::OpenMutexW(i 0x00100000, i 0, w "JeffPub79Running") p .r1'
    IntCmp $1 0 no_old_mutex
      System::Call 'kernel32::CloseHandle(p r1)'
      StrCpy $R9 1
  no_old_mutex:
    IfFileExists "$INSTDIR\${EXE}" 0 decide
      ClearErrors
      FileOpen $2 "$INSTDIR\${EXE}" a
      IfErrors locked
      FileClose $2
      Goto decide
    locked:
      StrCpy $R9 1
  decide:
    IntCmp $R9 0 not_running
      MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION "${APP} is running.$\r$\n$\r$\nSave your work and close ${APP}, then click OK to continue. Click Cancel to stop." IDOK check_running
      Abort
  not_running:
!macroend

Function .onInit
  !insertmacro WaitForAppToClose
  Call PresetStats
FunctionEnd

Function un.onInit
  !insertmacro WaitForAppToClose
FunctionEnd

Section "${APP} (required)" SecMain
  SectionIn RO
  Call RemoveJeffPub79
  SetOutPath "$INSTDIR"
  File /r "..\..\dist\JeffPub\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\${EXE}"
  WriteRegStr HKCU "Software\JeffOffice" "JeffPubInstallDir" "$INSTDIR"
  ; Usage statistics: off unless their section below is chosen, written where
  ; JeffPub keeps the choice. A silent install leaves it to the program,
  ; which asks when it first starts.
  IfSilent +2
  WriteRegStr HKCU "${SETKEY}\telemetry" "enabled" "false"
  ; Uninstall entry in Settings > Apps.
  WriteRegStr HKCU "${UNKEY}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNKEY}" "Publisher" "JeffOffice LLC"
  WriteRegStr HKCU "${UNKEY}" "DisplayIcon" "$INSTDIR\${EXE}"
  WriteRegStr HKCU "${UNKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNKEY}" "URLInfoAbout" "https://github.com/JeffOffice/jeffpub"
  WriteRegDWORD HKCU "${UNKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKCU "${UNKEY}" "EstimatedSize" $0
  ; .pub files list JeffPub under "Open with" without becoming the default.
  WriteRegStr HKCU "Software\Classes\JeffPub.PubFile" "" ".pub publication"
  WriteRegStr HKCU "Software\Classes\JeffPub.PubFile\DefaultIcon" "" "$INSTDIR\${EXE},0"
  WriteRegStr HKCU "Software\Classes\JeffPub.PubFile\shell\open\command" "" '"$INSTDIR\${EXE}" "%1"'
  WriteRegStr HKCU "Software\Classes\.pub\OpenWithProgids" "JeffPub.PubFile" ""
  WriteRegStr HKCU "Software\Classes\Applications\${EXE}\SupportedTypes" ".pub" ""
  WriteRegStr HKCU "Software\Classes\Applications\${EXE}\SupportedTypes" ".jpub" ""
SectionEnd

Section "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\${APP}.lnk" "$INSTDIR\${EXE}"
SectionEnd

Section "Open JeffPub publications (.jpub) with ${APP}" SecJpub
  WriteRegStr HKCU "Software\Classes\.jpub" "" "JeffPub.Publication"
  WriteRegStr HKCU "Software\Classes\JeffPub.Publication" "" "JeffPub publication"
  WriteRegStr HKCU "Software\Classes\JeffPub.Publication\DefaultIcon" "" "$INSTDIR\${EXE},0"
  WriteRegStr HKCU "Software\Classes\JeffPub.Publication\shell\open\command" "" '"$INSTDIR\${EXE}" "%1"'
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section /o "Make ${APP} the default for .pub files" SecPub
  WriteRegStr HKCU "Software\Classes\.pub" "" "JeffPub.PubFile"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section "Send anonymous usage statistics" SecStats
  IfSilent +2
  WriteRegStr HKCU "${SETKEY}\telemetry" "enabled" "true"
SectionEnd

; An update shows the choice already made: turned off in JeffPub, the
; statistics start unticked.
Function PresetStats
  ReadRegStr $0 HKCU "${SETKEY}\telemetry" "enabled"
  StrCmp $0 "" 0 +2
    ReadRegStr $0 HKCU "Software\JeffPub79\JeffPub79\telemetry" "enabled"
  StrCmp $0 "false" 0 +2
  SectionSetFlags ${SecStats} 0
FunctionEnd

; Takes away JeffPub 79 (0.5.0 and earlier): its program folder, shortcuts,
; file types and Settings > Apps entry. Its settings stay: JeffPub copies them
; the first time it starts. A .pub default that pointed at JeffPub 79 moves
; over, and so does a desktop shortcut.
Function RemoveJeffPub79
  ReadRegStr $0 HKCU "${OLDKEY}" "InstallLocation"
  StrCmp $0 "" types
  ; Only a folder that really holds JeffPub 79 is deleted.
  IfFileExists "$0\JeffPub79.exe" 0 entries
    StrCmp $0 $INSTDIR +2
      RMDir /r "$0"
  entries:
  Delete "$SMPROGRAMS\JeffPub 79.lnk"
  IfFileExists "$DESKTOP\JeffPub 79.lnk" 0 +3
    Delete "$DESKTOP\JeffPub 79.lnk"
    CreateShortcut "$DESKTOP\${APP}.lnk" "$INSTDIR\${EXE}"
  DeleteRegKey HKCU "${OLDKEY}"
  types:
  ReadRegStr $1 HKCU "Software\Classes\.pub" ""
  StrCmp $1 "JeffPub79.PubFile" 0 +2
    WriteRegStr HKCU "Software\Classes\.pub" "" "JeffPub.PubFile"
  ReadRegStr $1 HKCU "Software\Classes\.jpub" ""
  StrCmp $1 "JeffPub79.Publication" 0 +2
    WriteRegStr HKCU "Software\Classes\.jpub" "" "JeffPub.Publication"
  DeleteRegValue HKCU "Software\Classes\.pub\OpenWithProgids" "JeffPub79.PubFile"
  DeleteRegKey HKCU "Software\Classes\JeffPub79.PubFile"
  DeleteRegKey HKCU "Software\Classes\JeffPub79.Publication"
  DeleteRegKey HKCU "Software\Classes\Applications\JeffPub79.exe"
  DeleteRegValue HKCU "Software\JeffPub79" "InstallDir"
FunctionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "The program, its fonts and its spelling dictionaries."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStats} "Once a day (and when updated), ${APP} sends its version, your operating system and language, and how often each of its commands is used. Never your files, their names, or anything in them. You can change this later in File > Options."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Adds a shortcut to your desktop."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecJpub} "Double-clicking a .jpub file opens it in ${APP}."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPub} "Double-clicking a .pub file opens it in ${APP} instead of the program that opens it now. Leave this off to keep that program as the default; ${APP} is always available under Open with."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  Delete "$SMPROGRAMS\${APP}.lnk"
  Delete "$DESKTOP\${APP}.lnk"
  RMDir /r "$INSTDIR"
  ReadRegStr $0 HKCU "Software\Classes\.pub" ""
  StrCmp $0 "JeffPub.PubFile" 0 +2
    DeleteRegValue HKCU "Software\Classes\.pub" ""
  ReadRegStr $0 HKCU "Software\Classes\.jpub" ""
  StrCmp $0 "JeffPub.Publication" 0 +2
    DeleteRegKey HKCU "Software\Classes\.jpub"
  DeleteRegValue HKCU "Software\Classes\.pub\OpenWithProgids" "JeffPub.PubFile"
  DeleteRegKey HKCU "Software\Classes\JeffPub.PubFile"
  DeleteRegKey HKCU "Software\Classes\JeffPub.Publication"
  DeleteRegKey HKCU "Software\Classes\Applications\${EXE}"
  DeleteRegKey HKCU "${UNKEY}"
  DeleteRegKey HKCU "${SETKEY}"
  DeleteRegValue HKCU "Software\JeffOffice" "JeffPubInstallDir"
  DeleteRegKey /ifempty HKCU "Software\JeffOffice"
  DeleteRegKey HKCU "Software\JeffPub79"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd
