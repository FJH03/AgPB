@echo off
py -B "%~dp0agpw_view3d.py" %*
if errorlevel 1 pause
