@echo off
rem 結（yui）を Windows でビルドします。
rem
rem   build.bat          yui.exe を作る
rem   build.bat clean    消す
rem
rem Visual Studio 2022（C++ ツールセット）が要ります。外部のライブラリは要りません。
rem ★/utf-8 は必須です。付けないと MSVC がソースを cp932 と解釈し、
rem   日本語の文字列が壊れます。

setlocal

if "%1"=="clean" (
  del /q yui.exe *.obj *.pdb *.ilk 2>nul
  echo 消しました
  exit /b 0
)

rem vcvars64.bat を探します。既定の場所に無ければ vswhere で探します。
set "VCVARS="
for %%P in (
  "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) do if exist %%P set "VCVARS=%%~P"

if not defined VCVARS (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
      if exist "%%I\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
    )
  )
)

if not defined VCVARS (
  echo エラー: Visual Studio の vcvars64.bat が見つかりません。
  echo         Visual Studio 2022 の「C++ によるデスクトップ開発」を入れてください。
  exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
  echo エラー: vcvars64.bat の実行に失敗しました
  exit /b 1
)

cl /nologo /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS ^
   /Fe:yui.exe yui.c yui_cli.c
if errorlevel 1 (
  echo ビルドに失敗しました
  exit /b 1
)

echo できました: yui.exe
endlocal
