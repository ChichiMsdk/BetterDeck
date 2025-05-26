:: -- Script setup --------------------------------------------------------------------
@echo off
cd /D "%~dp0"
setlocal
if exist C:\vc_env\msvc\setup.bat @call "C:\vc_env\msvc\setup.bat"

where /q cl || (
  echo ERROR: "cl" not found - please run from MSVC x64 native tools command prompt.
  rem exit /b 1
)

where /q clang || (
  echo ERROR: "clang" not found - please install llvm-toolchain to use CLANG.
  exit /b 1
)

:: -- Get command-line arguments -----------------------------------------------------
for %%a in (%*) do set "%%a=1"
if not "%msvc%"=="1" if not "%clang%"=="1" set msvc=1
if not "%release%"=="1" set debug=1
if "%debug%"=="1"   set release=0 && echo [debug mode]
if "%release%"=="1" set debug=0 && echo [release mode]
if "%msvc%"=="1"    set clang=0 && echo [msvc compile]
if "%clang%"=="1"   set msvc=0 && echo [clang compile]
rem if not %crt%=1

:: -- CRT_NO_CRT ----------------------------------------------------------------------

if not "%crt%"=="1" (
  echo [NO_CRT LINKED]
  set def_crt=/DNO_CRT_LINKED
  set cc_nocrt=/Zl /Gs9999999
  set l_nocrt=/STACK:0x100000,0x100000
  set crt_libs=vcruntime.lib ucrt.lib
)
if "%crt%"=="1" (
  set def_crt=/DCRT_LINKED
  set cc_asan=/fsanitize=address
  set cc_nocrt=
  set l_nocrt=
)

:: -- Common options -----------------------------------------------------------------
set src_dir=src
set program=streamdeck
set source=main
set bin=bin
set ext=.exe
set out_path=%bin%\%program%
set out_path_debug=%bin%\%program%_debug

:: -- Resource options ---------------------------------------------------------------
set assets=assets
set rc_res=%bin%\resource.res
set rsc_out=/fo %rc_res%
set rc_file=%assets%\resource.rc
set rc_includes=/i %src_dir%\
set rc=rc.exe /NOLOGO

:: -- Scaffolding ---------------------------------------------------------------------
if not exist %bin% md %bin%
if exist %out_path%.* del %out_path%.*
if exist %out_path_debug%.* del %out_path_debug%.*
if exist assets if not exist %rc_res% %rc% %rc_includes% %rsc_out% %rc_file%

:: -- MSVC compiler options -----------------------------------------------------------
set no_cpp_options=/EHsc /EHa- /GR-
set cc_secure=/GS- /guard:cf-
set cc_options=/options:strict %no_cpp_options% %cc_secure% /FC /c /Tc 
set cc_diag=/diagnostics:caret
set show_includes=/showIncludes
set show_includes=
set cc=cl %show_includes% /nologo %cc_diag% %cc_options%
set cc_includes=/I%src_dir%
rem set cc_def=/DDEBUG
set cc_def=%cc_def%
set cc_def=%cc_def% %def_crt%

:: -- (4090)const/volatile (4189)var init unused (5045)spectre mitigation -------------
set cc_w=/Wall /WX /wd4090 /wd4189 /wd5045
set cc_std=/experimental:c11atomics /std:clatest
set cc_opti=/Ot /Oi
set cc_dbg=/Zi

:: -- Normal build --------------------------------------------------------------------
set cc_pdb_out=/Fd:%out_path%.pdb
set cc_obj=%out_path%.obj
set cc_out=/Fo:%cc_obj% %cc_pdb_out%

:: -- Debug build ---------------------------------------------------------------------
set cc_pdb_out_debug=/Fd:%out_path_debug%.pdb
set cc_obj_debug=%out_path_debug%.obj
set cc_out_debug=/Fo:%cc_obj_debug% %cc_pdb_out_debug%

set cc_flags=%cc_w% %cc_def% %cc_dbg% %cc_nocrt% %cc_std% %cc_opti% %cc_includes% %cc_asan%

:: -- MSVC linker options ------------------------------------------------------------
set linker=link /nologo /INCREMENTAL:NO
set l_debug=/DEBUG:FULL /DYNAMICBASE:NO
set l_sys=/PROFILE /GUARD:NO
set l_arch=/MACHINE:X64 
set l_opt=/OPT:ICF /OPT:REF
set l_options=/WX %l_arch% %l_nocrt% %l_sys%

set l_files=/ILK:%out_path%.ilk /MAP:%out_path%.map /PDB:%out_path%.pdb 
set l_files_debug=/ILK:%out_path_debug%.ilk /MAP:%out_path_debug%.map /PDB:%out_path_debug%.pdb 
 
:: -- Libraries ----------------------------------------------------------------------
set win_libs=Shell32.lib Shlwapi.lib Kernel32.lib User32.lib
set hid_libs=CfgMgr32.lib SetupAPI.lib hid.lib
set libs=%win_libs% %d3d12libs% %hid_libs% %crt_libs%

set l_all=%l_debug% %l_options% %libs% %l_delay% %l_opti%
set l_out=/OUT:%out_path%%ext%
set l_out_debug=/OUT:%out_path_debug%%ext%

:: -- NORMAL BUILD -------------------------------------------------------------------
if "%release%"=="1" (
  %cc% %src_dir%\%source%.c %cc_flags% %cc_out%           || exit /b 1
  %linker% %cc_obj% %bin%\*.res %l_files% %l_out% %l_all% || exit /b 1
)
:: -- DEBUG BUILD --------------------------------------------------------------------
if "%debug%"=="1" (
  %cc% %src_dir%\%source%.c /DDEBUG %cc_flags% %cc_out_debug%               || exit /b 1
  %linker% %cc_obj_debug% %bin%\*.res %l_files_debug% %l_out_debug% %l_all% || exit /b 1
)

:: -- Misc ---------------------------------------------------------------------------
ctags -f tags --langmap=c:.c.h --languages=c -R src
