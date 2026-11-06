@echo off
rem Preprocess XbSymbolDatabase's signature tables into build\oovpa\*.i for tools\find_symbols.py.
rem Only the C preprocessor runs (cl /EP); nothing from the database is compiled or executed.
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set VCVARS=%%i
if not defined VCVARS (echo Visual Studio's C++ build tools were not found & exit /b 1)
call "%VCVARS%" >nul 2>&1
set DB=%~dp0..\third_party\XbSymbolDatabase
if not exist "%~dp0..\build\oovpa" mkdir "%~dp0..\build\oovpa"
cd /d "%~dp0..\build\oovpa"
for %%L in (D3D8 DSound Xapi XGraphic XActEng) do cl /nologo /EP /Zc:preprocessor /I"%DB%\include" /I"%DB%\src" /I"%DB%\src\lib" /I"%DB%\src\OOVPADatabase" "%DB%\src\OOVPADatabase\%%L_OOVPA.c" > %%L.i 2> %%L.err
