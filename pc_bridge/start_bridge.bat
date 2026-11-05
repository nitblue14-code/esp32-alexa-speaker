@echo off
chcp 65001 >nul
title Мост Spotify для колонки
cd /d "%~dp0"
python -X utf8 kolonka_bridge.py
pause
