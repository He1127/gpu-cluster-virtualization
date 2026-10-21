@echo off
setlocal
cd /d "%~dp0.."
if not exist build mkdir build
if not exist bin mkdir bin
if not exist results mkdir results
set "LAB_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%LAB_VSWHERE%" goto missing_vswhere
"%LAB_VSWHERE%" -latest -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > results\05_vs_install_path.txt
set "LAB_VS="
set /p "LAB_VS=" < results\05_vs_install_path.txt
if not defined LAB_VS goto missing_vs
if not exist "%LAB_VS%\VC\Auxiliary\Build\vcvars64.bat" goto missing_vs
call "%LAB_VS%\VC\Auxiliary\Build\vcvars64.bat" > results\05_build_environment.log 2>&1
if errorlevel 1 exit /b 1
if not defined LAB_CUDA set "LAB_CUDA=%CUDA_PATH_V12_6%"
if not defined LAB_CUDA set "LAB_CUDA=%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v12.6"
if not exist "%LAB_CUDA%\bin\nvcc.exe" goto missing_cuda
if not exist "%LAB_CUDA%\include\cuda.h" goto missing_cuda
if not exist "%LAB_CUDA%\lib\x64\cuda.lib" goto missing_cuda
if not exist build\tmp mkdir build\tmp
set "TEMP=%CD%\build\tmp"
set "TMP=%CD%\build\tmp"
"%LAB_CUDA%\bin\nvcc.exe" --use-local-env -arch=sm_89 -cubin -lineinfo -Xptxas=-v src\kernels.cu -o build\kernels.cubin > results\06_nvcc_cubin.log 2>&1
if errorlevel 1 exit /b 1
"%LAB_CUDA%\bin\nvcc.exe" --use-local-env -arch=compute_89 -ptx -lineinfo src\kernels.cu -o build\kernels.ptx > results\06_nvcc_ptx.log 2>&1
if errorlevel 1 exit /b 1
"%LAB_CUDA%\bin\cuobjdump.exe" --dump-sass build\kernels.cubin > results\06_sass.txt 2>&1
if errorlevel 1 exit /b 1
"%LAB_CUDA%\bin\cuobjdump.exe" --dump-resource-usage build\kernels.cubin > results\06_resources.txt 2>&1
if errorlevel 1 exit /b 1
cl /nologo /TC /W4 /Od /Zi /D_CRT_SECURE_NO_WARNINGS /I"%LAB_CUDA%\include" /Fo:build\gpu_labs.obj /Fd:build\gpu_compile.pdb src\gpu_labs.c /Fe:bin\gpu_labs.exe /link /DEBUG /PDB:bin\gpu_labs.pdb /LIBPATH:"%LAB_CUDA%\lib\x64" cuda.lib > results\05_build_c.log 2>&1
if errorlevel 1 exit /b 1
powershell.exe -NoProfile -Command "[Console]::WriteLine('path,sha256'); foreach($labFile in @('src\gpu_labs.c','src\kernels.cu','build\kernels.cubin','build\kernels.ptx','bin\gpu_labs.exe')) { $labHash=[Security.Cryptography.SHA256]::Create().ComputeHash([IO.File]::ReadAllBytes(([IO.Path]::GetFullPath($labFile)))); [Console]::WriteLine($labFile+','+[BitConverter]::ToString($labHash).Replace('-','')) }" > results\05_source_hashes.csv
exit /b %errorlevel%
:missing_vswhere
echo ERROR: vswhere.exe was not found. Install Visual Studio 2022 with Desktop development with C++.
exit /b 2
:missing_vs
echo ERROR: Visual Studio 2022 x64 C++ tools were not found. Add the MSVC v143 and Windows SDK components.
exit /b 2
:missing_cuda
echo ERROR: CUDA Toolkit 12.6 was not found. Install it, or set LAB_CUDA to its installation directory.
exit /b 2