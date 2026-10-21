@echo off
rem Builds pxview (pixiv only). Usage: build-px.bat  /  build-px.bat cia  /  build-px.bat clean
call "%~dp0build.bat" SITE=pixiv %*
