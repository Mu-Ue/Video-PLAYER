@echo off
setlocal
cd /d "%~dp0"

rem Uses the exe passed as an argument, else one next to this script, else the build output.
set "EXE=%~f1"
if not defined EXE if exist VideoPLAYER.exe set "EXE=%~dp0VideoPLAYER.exe"
if not defined EXE set "EXE=%~dp0build\VideoPLAYER.exe"
if not exist "%EXE%" (
    echo VideoPLAYER.exe was not found. Run build.bat first, or pass the exe path.
    exit /b 1
)

start "" /wait "%EXE%" --unregister
start "" /wait "%EXE%" --register || (
    echo Registration failed.
    exit /b 1
)

set "PROGRAMS=%APPDATA%\Microsoft\Windows\Start Menu\Programs"
set "LNK=%PROGRAMS%\VideoPLAYER.lnk"
rem Removes any other shortcut to a VideoPLAYER.exe, and only rewrites ours when it is
rem missing or points to a different exe.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$sh = New-Object -ComObject WScript.Shell;" ^
    "Get-ChildItem -LiteralPath $env:PROGRAMS -Filter *.lnk -Recurse | Where-Object { $_.FullName -ne $env:LNK } | ForEach-Object {" ^
    "  $t = $sh.CreateShortcut($_.FullName).TargetPath;" ^
    "  if ($t -and (Split-Path $t -Leaf) -eq 'VideoPLAYER.exe') { Remove-Item -LiteralPath $_.FullName; 'Removed old shortcut ' + $_.FullName }" ^
    "};" ^
    "$s = $sh.CreateShortcut($env:LNK);" ^
    "if ($s.TargetPath -eq $env:EXE) { 'The Start menu shortcut is already up to date.'; exit 0 }" ^
    "$s.TargetPath = $env:EXE;" ^
    "$s.WorkingDirectory = Split-Path $env:EXE;" ^
    "$s.IconLocation = $env:EXE + ',0';" ^
    "$s.Save();" ^
    "'Added VideoPLAYER to the Start menu.'" || (
    echo Could not create the Start menu shortcut.
    exit /b 1
)

echo Registered %EXE%
