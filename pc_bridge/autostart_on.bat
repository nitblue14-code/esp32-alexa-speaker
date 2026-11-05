@echo off
chcp 65001 >nul
rem Мост Spotify будет сам запускаться при входе в Windows — свёрнутым окном на панели задач.
rem Отключить: autostart_off.bat
set "VBS=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\Kolonka Spotify bridge.vbs"
> "%VBS%" echo CreateObject("WScript.Shell").Run """%~dp0start_bridge.bat""", 7, False
echo Автозапуск моста включён: "%VBS%"
