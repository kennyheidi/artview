@echo off
rem Builds derpiview (the Derpibooru version). Usage: build-derpi.bat  /  build-derpi.bat cia  /  build-derpi.bat clean
call "%~dp0build.bat" SITE=derpibooru %*
