@echo off
setlocal
cd /d "%~dp0.."
if not exist bin mkdir bin
if not exist build mkdir build
set "SOCKET_VIEWER_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%SOCKET_VIEWER_VSWHERE%" goto missing_vs
"%SOCKET_VIEWER_VSWHERE%" -latest -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > build\viewer_vs_path.txt
set "SOCKET_VIEWER_VS="
set /p "SOCKET_VIEWER_VS=" < build\viewer_vs_path.txt
if not defined SOCKET_VIEWER_VS goto missing_vs
call "%SOCKET_VIEWER_VS%\VC\Auxiliary\Build\vcvars64.bat" > build\viewer_environment.log 2>&1
if errorlevel 1 exit /b 1
cl /nologo /TC /std:c11 /O2 /W4 /DUNICODE /D_UNICODE /utf-8 /Fo:build\result_viewer.obj src\result_viewer.c /Fe:bin\result_viewer.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib > build\viewer_compile.log 2>&1
if errorlevel 1 goto failed
echo VIEWER BUILD PASS: bin\result_viewer.exe
exit /b 0
:missing_vs
echo ERROR: Visual Studio 2022 x64 C tools not found.
exit /b 2
:failed
type build\viewer_compile.log
echo VIEWER BUILD FAILED
exit /b 1
