@echo off
setlocal
cd /d "%~dp0.."
if not exist build mkdir build
if not exist bin mkdir bin
if not exist results mkdir results
set "LAB_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%LAB_VSWHERE%" goto missing_vs
"%LAB_VSWHERE%" -latest -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > results\vs_path.txt
set "LAB_VS="
set /p "LAB_VS=" < results\vs_path.txt
if not defined LAB_VS goto missing_vs
call "%LAB_VS%\VC\Auxiliary\Build\vcvars64.bat" > results\build_environment.log 2>&1
if errorlevel 1 exit /b 1
if not defined LAB_CUDA set "LAB_CUDA=%CUDA_PATH_V12_6%"
if not defined LAB_CUDA set "LAB_CUDA=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6"
if not exist "%LAB_CUDA%\bin\nvcc.exe" goto missing_cuda
if not exist build\tmp mkdir build\tmp
set "TEMP=%CD%\build\tmp"
set "TMP=%CD%\build\tmp"
"%LAB_CUDA%\bin\nvcc.exe" --version > results\nvcc_version.txt 2>&1
"%LAB_CUDA%\bin\nvcc.exe" --use-local-env -O3 -arch=sm_89 -cubin -lineinfo -Xptxas=-v src\kernels.cu -o build\kernels.cubin > results\build_kernels.log 2>&1
if errorlevel 1 goto failed
"%LAB_CUDA%\bin\nvcc.exe" --use-local-env -O3 -arch=compute_89 -ptx src\kernels.cu -o build\kernels.ptx > results\build_ptx.log 2>&1
if errorlevel 1 goto failed
cl /nologo /TC /std:c11 /O2 /W4 /D_CRT_SECURE_NO_WARNINGS /I"%LAB_CUDA%\include" /Fo:build\api_stress.obj src\api_stress.c /Fe:bin\api_stress.exe /link /LIBPATH:"%LAB_CUDA%\lib\x64" cuda.lib > results\build_host.log 2>&1
if errorlevel 1 goto failed
"%LAB_CUDA%\bin\cuobjdump.exe" --dump-sass build\kernels.cubin > results\kernels_sass.txt 2>&1
if errorlevel 1 exit /b 1
echo BUILD PASS: C /TC /O2 host + CUDA sm_89 cubin
exit /b 0
:missing_vs
echo ERROR: Visual Studio 2022 x64 C++ tools not found.
exit /b 2
:missing_cuda
echo ERROR: CUDA 12.6 nvcc not found.
exit /b 2
:failed
echo BUILD FAILED. Check results\build_kernels.log, build_ptx.log and build_host.log.
exit /b 1
