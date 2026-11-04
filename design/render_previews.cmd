@echo off
setlocal
set "TEMP=E:\zx\cache\tmp\codex_tsafari_ui"
set "TMP=E:\zx\cache\tmp\codex_tsafari_ui"
set "PYTHONDONTWRITEBYTECODE=1"
cd /d "%~dp0"
"E:\zx\esp\pio-venv\Scripts\python.exe" -B "%~dp0render_previews.py"
exit /b %errorlevel%
