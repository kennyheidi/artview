@echo off
rem Builds e6view (e621 only). Usage: build-e6.bat  /  build-e6.bat cia  /  build-e6.bat clean
call "%~dp0build.bat" SITE=e621 %*
