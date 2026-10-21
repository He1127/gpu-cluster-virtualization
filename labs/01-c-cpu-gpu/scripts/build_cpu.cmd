@echo off
setlocal
cd /d "%~dp0.."
if not exist bin mkdir bin
if not exist results mkdir results
set "LAB_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%LAB_VSWHERE%" goto missing_vswhere
"%LAB_VSWHERE%" -latest -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > results\01_vs_install_path.txt
set "LAB_VS="
set /p "LAB_VS=" < results\01_vs_install_path.txt
if not defined LAB_VS goto missing_vs
if not exist "%LAB_VS%\VC\Auxiliary\Build\vcvars64.bat" goto missing_vs
call "%LAB_VS%\VC\Auxiliary\Build\vcvars64.bat" > results\01_build_environment.log 2>&1
if errorlevel 1 exit /b %errorlevel%
cl /nologo /TC /W4 /Od /Ob0 /Zi /FAs /Fa"results\02_cpu_Od.asm" /Fo"bin\cpu_Od.obj" /Fd"bin\cpu_Od_compile.pdb" /D_CRT_SECURE_NO_WARNINGS src\cpu_labs.c /Fe:"bin\cpu_labs_Od.exe" /link /DEBUG /PDB:"bin\cpu_labs_Od.pdb" /MAP:"results\02_cpu_Od.map" advapi32.lib > results\01_build_Od.log 2>&1
if errorlevel 1 exit /b %errorlevel%
cl /nologo /TC /W4 /O2 /Zi /DOPTIMIZED_BUILD /FAs /Fa"results\02_cpu_O2.asm" /Fo"bin\cpu_O2.obj" /Fd"bin\cpu_O2_compile.pdb" /D_CRT_SECURE_NO_WARNINGS src\cpu_labs.c /Fe:"bin\cpu_labs_O2.exe" /link /DEBUG /PDB:"bin\cpu_labs_O2.pdb" /MAP:"results\02_cpu_O2.map" advapi32.lib > results\02_build_O2.log 2>&1
if errorlevel 1 exit /b %errorlevel%
dumpbin /DISASM bin\cpu_Od.obj > results\02_cpu_Od_object_disassembly.txt 2>&1
if errorlevel 1 exit /b %errorlevel%
dumpbin /DISASM bin\cpu_O2.obj > results\02_cpu_O2_object_disassembly.txt 2>&1
if errorlevel 1 exit /b %errorlevel%
dumpbin /DISASM bin\cpu_labs_Od.exe > results\02_cpu_Od_executable_disassembly.txt 2>&1
if errorlevel 1 exit /b %errorlevel%
dumpbin /DISASM bin\cpu_labs_O2.exe > results\02_cpu_O2_executable_disassembly.txt 2>&1
exit /b %errorlevel%
:missing_vswhere
echo ERROR: vswhere.exe was not found. Install Visual Studio 2022 with Desktop development with C++.
exit /b 2
:missing_vs
echo ERROR: Visual Studio 2022 x64 C++ tools were not found. Add the MSVC v143 and Windows SDK components.
exit /b 2