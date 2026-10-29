@echo off
rem Сборка MSVC: запускать из "x64 Native Tools Command Prompt for VS"
rem Используются заголовки WPD из Windows SDK, поэтому portabledevice*.h из этой папки не нужны.
mkdir sdk_only 2>nul
copy /y FarMTP.cpp sdk_only\ >nul
copy /y FarMTP.def sdk_only\ >nul
copy /y plugin.hpp sdk_only\ >nul
cd sdk_only
cl /nologo /O2 /EHsc /LD /DUNICODE /D_UNICODE /D_export= FarMTP.cpp /link /DEF:FarMTP.def ole32.lib oleaut32.lib uuid.lib /OUT:..\FarMTP.dll
cd ..
