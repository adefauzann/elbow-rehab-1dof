@echo off
title Dashboard Rehabilitasi Sendi Siku - Server
color 0A

echo ============================================
echo   Dashboard Rehabilitasi Sendi Siku 1-DOF
echo   Server sedang dijalankan...
echo ============================================
echo.
echo [INFO] Mengaktifkan server di http://localhost:8000
echo [INFO] Tekan CTRL+C untuk menghentikan server.
echo.

cd /d "%~dp0"
.venv\Scripts\python.exe main.py

echo.
echo [INFO] Server berhenti. Tekan tombol apa saja untuk keluar.
pause > nul
