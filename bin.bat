@taskkill /IM QuickRun.exe /F 2>nul
@REM windres app.rc -o app_res.o
@windres app.rc -o app_res.o --codepage=65001
gcc QuickRun.c app_res.o -o QuickRun.exe -Os -mwindows -s -lshell32 -ldwmapi -lmsimg32

