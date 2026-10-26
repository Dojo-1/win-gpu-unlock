@echo off
rem Explicit build environment (vcvars broken by CUDA VS integration).
set "MSVC=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
set "KIT=C:\Program Files (x86)\Windows Kits\10"
set "SDK=10.0.26100.0"
set "INCLUDE=%MSVC%\include;%KIT%\Include\%SDK%\km;%KIT%\Include\%SDK%\km\crt;%KIT%\Include\%SDK%\shared;%KIT%\Include\%SDK%\ucrt;%KIT%\Include\%SDK%\um"
set "LIB=%MSVC%\lib\x64;%KIT%\Lib\%SDK%\km\x64;%KIT%\Lib\%SDK%\ucrt\x64;%KIT%\Lib\%SDK%\um\x64"
set "PATH=%MSVC%\bin\Hostx64\x64;C:\Windows\System32;C:\Windows;C:\Program Files\Git\cmd"
