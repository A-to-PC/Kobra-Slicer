@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set PATH=H:\VS Code\My Apps\Kobra-Slicer\tools\cmake-3.31.6-windows-x86_64\bin;C:\Strawberry\perl\bin;%PATH%
cd /d "H:\VS Code\My Apps\Kobra-Slicer"

set CMAKE_POLICY_VERSION_MINIMUM=3.5
mkdir build 2>nul
cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DORCA_TOOLS=ON -DCMAKE_BUILD_TYPE=Release
echo CONFIGURE EXIT CODE: %ERRORLEVEL%
cmake --build . --config Release --target ALL_BUILD -- -m:4
echo BUILD EXIT CODE: %ERRORLEVEL%
