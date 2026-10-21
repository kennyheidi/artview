@echo off
rem Builds r34view (rule34.xxx only). Usage: build-r34.bat  /  build-r34.bat cia  /  build-r34.bat clean
call "%~dp0build.bat" SITE=rule34 %*
