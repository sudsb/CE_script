@ECHO OFF
SETLOCAL ENABLEEXTENSIONS
:: encoding: UTF-8
CHCP 65001 >NUL 2>&1

rem ******************************************************************************
rem *                                                                            *
rem * Notepad3                                                                   *
rem *                                                                            *
rem * BuildAndPackage.cmd                                                        *
rem *   Build Win32 + x64 (incl. minipath / np3encrypt) and create the           *
rem *   "Portable (*.zip)" package. Wraps the existing Build_*.cmd and           *
rem *   make_portable_rel(.zip).cmd scripts, so the result is identical to a     *
rem *   manual build + package.                                                  *
rem *                                                                            *
rem * Usage:  BuildAndPackage.cmd [Release|Debug]                                *
rem *                                                                            *
rem ******************************************************************************

PUSHD %~dp0

SET "CONFIG=%~1"
IF "%CONFIG%" == "" SET "CONFIG=Release"

ECHO.
ECHO ============================================================
ECHO  Notepad3 - Build + Package  (%CONFIG%)
ECHO ============================================================

CALL :SUBMSG "INFO" "Building Win32 (%CONFIG%) ..."
CALL Build_Win32.cmd %CONFIG%
IF %ERRORLEVEL% NEQ 0 GOTO :BUILD_FAIL

CALL :SUBMSG "INFO" "Building x64 (%CONFIG%) ..."
CALL Build_x64.cmd %CONFIG%
IF %ERRORLEVEL% NEQ 0 GOTO :BUILD_FAIL

CALL :SUBMSG "INFO" "Packaging portable (*.zip) ..."
CALL "make_portable_rel(.zip).cmd"
IF %ERRORLEVEL% NEQ 0 GOTO :PKG_FAIL

CALL :SUBMSG "INFO" "Package created. See Build\packages\ (or Build root)."
GOTO :END


:BUILD_FAIL
CALL :SUBMSG "ERROR" "Build failed - aborting packaging."
GOTO :END

:PKG_FAIL
CALL :SUBMSG "ERROR" "Packaging failed (missing binaries or grepWin portables?)."
GOTO :END


:END
ECHO.
ECHO Done.
POPD
ENDLOCAL
EXIT /B


:SUBMSG
ECHO.
ECHO ______________________________
ECHO [%~1] %~2
ECHO ______________________________ & ECHO.
IF /I "%~1" == "ERROR" ( PAUSE & EXIT /B 1 )
EXIT /B 0
