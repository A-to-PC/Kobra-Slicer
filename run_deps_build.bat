@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set PATH=H:\VS Code\My Apps\Kobra-Slicer\tools\cmake-3.31.6-windows-x86_64\bin;C:\Strawberry\perl\bin;%PATH%
cd /d "H:\VS Code\My Apps\Kobra-Slicer"
echo CURRENT DIR IS: %CD%
dir build_release_vs2022.bat
call "H:\VS Code\My Apps\Kobra-Slicer\build_release_vs2022.bat" deps
echo BUILD SCRIPT EXIT CODE: %ERRORLEVEL%
