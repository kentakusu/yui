@echo off
rem 結（yui）を Windows でビルドします。
rem
rem   build.bat          yui.exe を作る
rem   build.bat test     テストを作って流す
rem   build.bat clean    消す
rem
rem Visual Studio 2022（C++ ツールセット）が要ります。外部のライブラリは要りません。
rem ★/utf-8 は必須です。付けないと MSVC がソースを cp932 と解釈し、
rem   日本語の文字列が壊れます。
rem ★chcp 65001 も必須です。付けないと、このファイルの日本語と
rem   プログラムが出す日本語が、コンソールで化けます。
rem ★実行ファイルは .\ を付けて呼びます。NoDefaultCurrentDirectoryInExePath が
rem   立っている環境では、カレントディレクトリを探してくれません。

chcp 65001 >nul
setlocal enabledelayedexpansion

if "%1"=="clean" (
  del /q yui.exe yui_test.exe *.obj *.pdb *.ilk 2>nul
  del /q yui_test_tmp.yui "yui_test_tmp.yui.断片" "yui_test_tmp.yui.詰" "yui_test_tmp.yui.錠" 2>nul
  echo 消しました
  exit /b 0
)

rem vcvars64.bat を探します。既定の場所に無ければ vswhere で探します。
rem ★( ) のブロックの中に %ProgramFiles(x86)% を書かないこと。
rem   中の ) がブロックの終わりと解釈され、中身が勝手に走ります。
set "VCVARS="
for %%P in (
  "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) do if exist %%P set "VCVARS=%%~P"
if defined VCVARS goto :vcvars_ok

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" goto :no_compiler
for /f "usebackq delims=" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do if exist "%%I\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
if defined VCVARS goto :vcvars_ok

:no_compiler
echo エラー: Visual Studio の vcvars64.bat が見つかりません。
echo         Visual Studio 2022 の「C++ によるデスクトップ開発」を入れてください。
exit /b 1

:vcvars_ok
call "!VCVARS!" >nul
if errorlevel 1 (
  echo エラー: vcvars64.bat の実行に失敗しました
  exit /b 1
)

set "CFLAGS=/nologo /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS"

if "%1"=="test" goto :test

cl %CFLAGS% /Fe:yui.exe yui.c yui_cli.c
if errorlevel 1 (
  echo ビルドに失敗しました
  exit /b 1
)
echo できました: yui.exe
exit /b 0

:test
rem ケースは tests\cases\*.txt。書き方は tests\README.md。
rem /DYUI_SELFCHECK: 探すたびに索引と全件走査の答えを突き合わせます（テストだけ）
cl %CFLAGS% /DYUI_SELFCHECK /Fe:yui_test.exe yui.c tests\test_yui.c
if errorlevel 1 (
  echo ビルドに失敗しました
  exit /b 1
)
set "CASES="
for %%F in (tests\cases\*.txt) do set "CASES=!CASES! tests\cases\%%~nxF"
if not defined CASES (
  echo エラー: tests\cases\ にケースがありません
  exit /b 1
)
.\yui_test.exe !CASES!
exit /b !errorlevel!
