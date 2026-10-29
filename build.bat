@echo off
rem Builds dist\LangFix.exe (no CRT) and the test tools build\lftest.exe and build\e2e.exe.
setlocal
cd /d "%~dp0"
if not defined VCINSTALLDIR (
  set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
  for /f "usebackq delims=" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
)
if not defined VCINSTALLDIR call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build mkdir build
if not exist dist mkdir dist

python -I tools\build_model.py || exit /b 1
if not exist res\on.ico powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_icons.ps1 || exit /b 1

rc /nologo /fo build\app.res res\app.rc || exit /b 1
cl /nologo /c /O1 /Os /GS- /Gs999999 /Zl /W4 /DUNICODE /D_UNICODE /Fobuild\ src\engine.c src\main.c src\ui.c src\ai.c || exit /b 1
link /nologo /NODEFAULTLIB /ENTRY:start /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /EMITPOGOPHASEINFO /MANIFEST:NO ^
  /out:dist\LangFix.exe build\engine.obj build\main.obj build\ui.obj build\ai.obj build\app.res ^
  kernel32.lib user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib comctl32.lib uxtheme.lib ^
  winhttp.lib crypt32.lib || exit /b 1

if not exist build\t mkdir build\t
cl /nologo /O2 /W4 /DUNICODE /D_UNICODE /Fobuild\t\ /Fe:build\lftest.exe tests\test.c src\engine.c ole32.lib user32.lib >nul || exit /b 1
cl /nologo /O2 /W3 /DUNICODE /D_UNICODE /Fobuild\t\ /Fe:build\e2e.exe tests\e2e.c user32.lib advapi32.lib >nul || exit /b 1
for %%f in (dist\LangFix.exe) do echo built %%~f (%%~zf bytes)
