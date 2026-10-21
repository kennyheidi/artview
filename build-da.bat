@echo off
rem Builds daview (the DeviantArt version). Usage: build-da.bat  /  build-da.bat cia  /  build-da.bat clean
call "%~dp0build.bat" SITE=deviantart %*
