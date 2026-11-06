@echo off
setlocal
cd /d "%~dp0.."
if not exist bin mkdir bin
if not exist build mkdir build
set "LAB_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%LAB_VSWHERE%" goto missing
"%LAB_VSWHERE%" -latest -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > build\vs_path.txt
set "LAB_VS="
set /p "LAB_VS=" < build\vs_path.txt
if not defined LAB_VS goto missing
call "%LAB_VS%\VC\Auxiliary\Build\vcvars64.bat" > build\environment.log 2>&1
if errorlevel 1 exit /b 1
cl /nologo /TC /std:c11 /O2 /W4 /D_CRT_SECURE_NO_WARNINGS src\socket_lab.c /Fo:build\socket_lab.obj /Fe:bin\socket_lab.exe /link ws2_32.lib > build\compile.log 2>&1
if errorlevel 1 goto failed
type build\compile.log
echo BUILD PASS: x64 C11 Winsock overlapped IOCP
exit /b 0
:missing
echo ERROR: Visual Studio 2022 x64 C++ tools are required.
exit /b 2
:failed
type build\compile.log
echo BUILD FAILED
exit /b 1
