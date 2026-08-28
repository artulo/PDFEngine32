@ECHO OFF
REM ============================================================================
REM Build.bat - PDFEngine32 + Harbour/FiveWin
REM
REM Este script esta clonado de tu build.bat real de FWH2603 (el que me
REM pasaste) -- mismas rutas, mismas libs (psdk, lib\win\bcc), mismo GT=gtgui.
REM Lo unico que se agrega es: compilar los .c del motor PDFEngine32 y el
REM glue harbour\pdf_hbfunc.c ANTES del paso de Harbour/link, y meter esos
REM .obj en la misma lista de objetos que %PRG%.obj.
REM
REM ADVERTENCIA REAL CONFIRMADA (le costo mucho tiempo a una sesion
REM detectar esto): en esta maquina, este script A VECES compila
REM pdf_object.c y/o pdf_parser.c contra contenido VIEJO pese a imprimir
REM "Listo" al final y pese a que el .obj resultante queda con timestamp
REM nuevo -- el mismo sabor del problema historico de "Could not find
REM file"/rutas documentado mas abajo, pero MAS peligroso porque no da
REM ningun error, solo compila mal en silencio. Confirmado comparando
REM el .obj resultante contra uno compilado a mano con el MISMO comando
REM bcc32 (mismas flags, mismo archivo fuente) fuera de este script --
REM salen binariamente DISTINTOS. Si haces cambios en pdf_object.c o
REM pdf_parser.c y el resultado no refleja el cambio, no confies en
REM "Listo" -- recompila esos DOS archivos a mano con bcc32 directo
REM (mismas flags que CFLAGS_ENG_NOOPT mas abajo) y copia el .obj
REM resultante encima antes de linkear.
REM ============================================================================

if "%FWDIR%" == "" set FWDIR=d:\prgsmio\FWH2603
if "%HBDIR%" == "" set HBDIR=%FWDIR%\harbour
set GT=gtgui

set hdir=%HBDIR%
set hdirl=%hdir%\lib\win\bcc
set fwh=%FWDIR%
set bcdir=d:\prgsmio\bcc770

set PRG=pdf_demo
set ENG=..
set ENGABS=D:\estudio\PDFEngine32

REM Borrar .obj viejos: si una compilacion falla mas abajo (por un warning
REM que tu BCC trata como error, por ejemplo), NO queremos que el link use
REM silenciosamente un .obj de una vuelta anterior -- mejor que falle clarito
REM por "no existe" a que enlace con codigo desactualizado sin avisar.
del /q *.obj 2>nul
del /q %PRG%.c %PRG%.ppo %PRG%.exe 2>nul
del /q pdf_viewer.c pdf_viewer.ppo 2>nul

ECHO %FWDIR%
ECHO %hdir%
ECHO %hdirl%

REM --- 1) Motor PDFEngine32 (los mismos .c que ya compilan limpio con bcc32) ---
REM
REM BUG REAL ENCONTRADO Y ARREGLADO (dos capas del mismo problema de fondo):
REM esta maquina tiene un problema reproducible de resolucion de rutas
REM relativas cuando bcc32 se invoca DESDE ESTE .bat especificamente (no en
REM una invocacion bcc32 suelta equivalente) -- no se identifico la causa
REM raiz exacta (sospecha: algun tipo de cache/enumeracion de directorio
REM stale de algo en el entorno, posiblemente sync de nube sobre D:\ -- ver
REM tambien el problema de Build.bat perdiendo su CRLF solo, con el mismo
REM sabor de "algo externo tocando archivos"). Se manifiesta de DOS formas:
REM   1. El ARGUMENTO del archivo fuente en si: "Error E2194: Could not
REM      find file" para algunos .c puntuales -- se arreglaba pasandolos
REM      con ruta ABSOLUTA (%ENGABS%) en vez de relativa (%ENG%, "..").
REM   2. El FLAG -I (rutas de include): confirmado esta ronda que bcc32
REM      resuelve un -I RELATIVO ("..\include") de forma DISTINTA segun si
REM      el argumento del archivo fuente es absoluto o relativo -- con
REM      fuente ABSOLUTA, el relativo "..\include" se resuelve contra el
REM      directorio DEL ARCHIVO FUENTE (no contra el cwd del proceso), asi
REM      que para un .c en src\core\, "..\include" cae en src\include (que
REM      no existe) en vez de la carpeta include\ real -- "Unable to open
REM      include file". Mezclar -I absoluto con SOLO ALGUNOS archivos en
REM      fuente relativa (probado y descartado) produce un sintoma peor
REM      todavia ("Undefined symbol 'ledger'" y similares -- headers
REM      equivocados). La solucion real es consistencia total: TODOS los
REM      archivos del motor de abajo usan %ENGABS% (fuente absoluta) Y el
REM      -I tambien absoluto, sin mezclar los dos estilos.
ECHO === Compilando motor PDFEngine32 ===

set CFLAGS_ENG=-c -O2 -I%ENGABS%\include -I%bcdir%\include
REM sin -O2, para pdf_object.c/pdf_parser.c -- ver el comentario grande
REM mas abajo, junto a donde se usa.
set CFLAGS_ENG_NOOPT=-c -I%ENGABS%\include -I%bcdir%\include

%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\core\pdf_mem.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\core\pdf_cache.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\core\pdf_context.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\core\pdf_device.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\core\pdf_display_list.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\document\pdf_document.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\document\pdf_aes.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\document\pdf_sha2.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\document\pdf_crypt.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\document\pdf_write.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\page\pdf_page.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\streams\pdf_stream.c
REM BUG REAL ENCONTRADO Y ARREGLADO (reportado por Arturo: PDFs con texto
REM "ilegible", patron de barras/rayas en vez de letras -- confirmado
REM que NO es un problema de renderizado de glyphs sino de resolucion
REM de referencias indirectas "N G R" corruptas ANTES de llegar a la
REM fuente siquiera). Una miscompilacion real de bcc32 7.70 (ver el
REM comentario grande en pdf_obj_new_ref, pdf_object.c, para el detalle
REM completo -- incluye como se aislo con un reproductor minimo sin
REM depender del PDF real). El fix que en la practica evita el problema
REM de forma reproducible (8/8 en el reproductor minimo, y confirmado
REM contra tests/Los_Kajchas_y_los_proyectos_de_industria.pdf) tiene DOS
REM partes obligatorias juntas: escritura via memcpy() en pdf_obj_new_ref
REM (ya en el .c) Y compilar pdf_object.c + pdf_parser.c SIN -O2 aca --
REM ninguna de las dos cosas sola alcanzaba. Los dos van SIEMPRE juntos
REM (probado: mezclar -O2 en uno solo de los dos crashea por mismatch de
REM convencion de llamada entre niveles de optimizacion en esta version
REM de bcc32).
%bcdir%\bin\bcc32 %CFLAGS_ENG_NOOPT% %ENGABS%\src\objects\pdf_object.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\parser\pdf_lexer.c
%bcdir%\bin\bcc32 %CFLAGS_ENG_NOOPT% %ENGABS%\src\parser\pdf_parser.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\parser\pdf_xref.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\render\pdf_colorspace.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\render\pdf_function.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\render\pdf_shading.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\filters\pdf_filter.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\filters\pdf_jpx.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\interpreter\pdf_content.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\text\pdf_font.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\text\pdf_afm.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\text\pdf_ttf.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\text\pdf_cff.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\text\pdf_text_extract.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\forms\pdf_form.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\annotations\pdf_annot.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\graphics\pdf_path.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\graphics\pdf_raster.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\image\pdf_image.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\render\pdf_bitmap.c
%bcdir%\bin\bcc32 %CFLAGS_ENG% %ENGABS%\src\render\pdf_render.c

if not exist pdf_render.obj goto ENGINEERROR
if not exist pdf_jpx.obj goto ENGINEERROR
if not exist pdf_ttf.obj goto ENGINEERROR
if not exist pdf_cff.obj goto ENGINEERROR
if not exist pdf_text_extract.obj goto ENGINEERROR
if not exist pdf_aes.obj goto ENGINEERROR
if not exist pdf_sha2.obj goto ENGINEERROR
if not exist pdf_form.obj goto ENGINEERROR
if not exist pdf_annot.obj goto ENGINEERROR
if not exist pdf_write.obj goto ENGINEERROR

REM --- 2) Glue Harbour (usa hbapi.h de Harbour + windows.h de BCC) -------------
ECHO === Compilando glue Harbour (pdf_hbfunc.c) ===

%bcdir%\bin\bcc32 -c -O2 -I%ENGABS%\include -I%hdir%\include -I%bcdir%\include %ENGABS%\harbour\pdf_hbfunc.c

if not exist pdf_hbfunc.obj goto ENGINEERROR

REM --- 3) pdf_viewer.prg (clase TPdfViewer -- navegacion de paginas + zoom,
REM     ver harbour\pdf_viewer.prg) -- se compila COMO MODULO APARTE, mismo
REM     patron que %PRG%.prg mas abajo (Harbour compila cada .prg a su
REM     propio .c/.obj; el linker resuelve las referencias cruzadas, igual
REM     que con los .c del motor). --------------------------------------

ECHO === Compilando pdf_viewer.prg ===
copy %ENGABS%\harbour\pdf_viewer.prg . > nul

%hdir%\bin\harbour pdf_viewer /n /i%fwh%\include;%hdir%\include /w /p > comp_viewer.log 2> warnings_viewer.log
if errorlevel 1 goto COMPILEERRORS_VIEWER
@type comp_viewer.log
@type warnings_viewer.log

echo -O2 -epdf_viewer.exe -I%hdir%\include -I%bcdir%\include -I%fwh%\include pdf_viewer.c > b32v.bc
%bcdir%\bin\bcc32 -M -c @b32v.bc

if not exist pdf_viewer.obj goto ENGINEERROR

REM --- 4) pdf_demo.prg (id??ntico a tu build.bat real, salvo el nombre) ---------
copy %ENGABS%\harbour\%PRG%.prg . > nul

ECHO Compiling...

%hdir%\bin\harbour %PRG% /n /i%fwh%\include;%hdir%\include /w /p > comp.log 2> warnings.log
if errorlevel 1 goto COMPILEERRORS
@type comp.log
@type warnings.log

echo -O2 -e%PRG%.exe -I%hdir%\include -I%bcdir%\include -I%fwh%\include %PRG%.c > b32.bc
%bcdir%\bin\bcc32 -M -c @b32.bc

REM --- 5) Link: objetos del motor + glue + pdf_viewer.obj + %PRG%.obj +
REM     TODAS las libs de tu build.bat real (sin recortar -- la version
REM     recortada dio unresolved externals porque FiveH.lib tiene
REM     referencias cruzadas a simbolos de hbrdd/hbct/xhb/etc. aunque el
REM     .prg no los use directamente).

echo -L%bcdir%\lib;%bcdir%\lib\psdk + > b32.bc
echo %bcdir%\lib\c0w32.obj + >> b32.bc
echo pdf_mem.obj + >> b32.bc
echo pdf_cache.obj + >> b32.bc
echo pdf_context.obj + >> b32.bc
echo pdf_device.obj + >> b32.bc
echo pdf_display_list.obj + >> b32.bc
echo pdf_document.obj + >> b32.bc
echo pdf_aes.obj + >> b32.bc
echo pdf_sha2.obj + >> b32.bc
echo pdf_crypt.obj + >> b32.bc
echo pdf_write.obj + >> b32.bc
echo pdf_page.obj + >> b32.bc
echo pdf_stream.obj + >> b32.bc
echo pdf_object.obj + >> b32.bc
echo pdf_lexer.obj + >> b32.bc
echo pdf_parser.obj + >> b32.bc
echo pdf_xref.obj + >> b32.bc
echo pdf_colorspace.obj + >> b32.bc
echo pdf_function.obj + >> b32.bc
echo pdf_shading.obj + >> b32.bc
echo pdf_filter.obj + >> b32.bc
echo pdf_jpx.obj + >> b32.bc
echo pdf_content.obj + >> b32.bc
echo pdf_font.obj + >> b32.bc
echo pdf_afm.obj + >> b32.bc
echo pdf_ttf.obj + >> b32.bc
echo pdf_cff.obj + >> b32.bc
echo pdf_text_extract.obj + >> b32.bc
echo pdf_form.obj + >> b32.bc
echo pdf_annot.obj + >> b32.bc
echo pdf_path.obj + >> b32.bc
echo pdf_raster.obj + >> b32.bc
echo pdf_image.obj + >> b32.bc
echo pdf_bitmap.obj + >> b32.bc
echo pdf_render.obj + >> b32.bc
echo pdf_hbfunc.obj + >> b32.bc
echo pdf_viewer.obj + >> b32.bc
echo %PRG%.obj, + >> b32.bc
echo %PRG%.exe, + >> b32.bc
echo %PRG%.map, + >> b32.bc
echo %fwh%\lib\FiveH.lib %fwh%\lib\FiveHC.lib %fwh%\lib\libmysql.lib + >> b32.bc
echo %fwh%\lib\xlsxlibhbbcc.lib + >> b32.bc
echo %fwh%\lib\hbpgsql.lib %fwh%\lib\libpq.lib + >> b32.bc
echo %fwh%\lib\drxlsx32_bcc.lib + >> b32.bc
echo %hdirl%\hbhpdf.lib + >> b32.bc
echo %hdirl%\libhpdf.lib + >> b32.bc
echo %hdirl%\png.lib + >> b32.bc
echo %hdirl%\hbwin.lib + >> b32.bc
echo %hdirl%\gtgui.lib + >> b32.bc
echo %hdirl%\hbrtl.lib + >> b32.bc
echo %hdirl%\hbvm.lib + >> b32.bc
echo %hdirl%\hblang.lib + >> b32.bc
echo %hdirl%\hbmacro.lib + >> b32.bc
echo %hdirl%\hbrdd.lib + >> b32.bc
echo %hdirl%\rddntx.lib + >> b32.bc
echo %hdirl%\rddcdx.lib + >> b32.bc
echo %hdirl%\rddfpt.lib + >> b32.bc
echo %hdirl%\hbsix.lib + >> b32.bc
echo %hdirl%\hbdebug.lib + >> b32.bc
echo %hdirl%\hbcommon.lib + >> b32.bc
echo %hdirl%\hbpp.lib + >> b32.bc
echo %hdirl%\hbcpage.lib + >> b32.bc
echo %hdirl%\hbcplr.lib + >> b32.bc
echo %hdirl%\hbct.lib + >> b32.bc
echo %hdirl%\hbpcre.lib + >> b32.bc
echo %hdirl%\xhb.lib + >> b32.bc
echo %hdirl%\hbziparc.lib + >> b32.bc
echo %hdirl%\hbmzip.lib + >> b32.bc
echo %hdirl%\hbzlib.lib + >> b32.bc
echo %hdirl%\minizip.lib + >> b32.bc
echo %hdirl%\hbusrrdd.lib + >> b32.bc
echo %hdirl%\hbtip.lib + >> b32.bc
echo %hdirl%\hbzebra.lib + >> b32.bc
echo %hdirl%\hbcurl.lib + >> b32.bc
echo %hdirl%\libcurl.lib + >> b32.bc
echo %fwh%\lib\dolphin.lib + >> b32.bc
echo %bcdir%\lib\cw32.lib + >> b32.bc
echo %bcdir%\lib\psdk\uuid.lib + >> b32.bc
echo %bcdir%\lib\import32.lib + >> b32.bc
echo %bcdir%\lib\psdk\ws2_32.lib + >> b32.bc
echo %bcdir%\lib\psdk\odbc32.lib + >> b32.bc
echo %bcdir%\lib\psdk\nddeapi.lib + >> b32.bc
echo %bcdir%\lib\psdk\iphlpapi.lib + >> b32.bc
echo %bcdir%\lib\psdk\msimg32.lib + >> b32.bc
echo %bcdir%\lib\psdk\psapi.lib + >> b32.bc
echo %bcdir%\lib\psdk\rasapi32.lib + >> b32.bc
echo %bcdir%\lib\psdk\gdiplus.lib + >> b32.bc
echo %bcdir%\lib\psdk\shell32.lib + >> b32.bc
echo %bcdir%\lib\psdk\uxtheme.lib , >> b32.bc

%bcdir%\bin\ilink32 -Gn -aa -Tpe -s @b32.bc
IF ERRORLEVEL 1 GOTO LINKERROR

ECHO * Listo: %PRG%.exe *
start %PRG%
GOTO EXIT

:ENGINEERROR
ECHO.
ECHO *** Fallo la compilacion del motor o del glue -- revisar arriba ***
GOTO EXIT

:COMPILEERRORS_VIEWER
@type comp_viewer.log
@type warnings_viewer.log
ECHO * Errores de compilacion Harbour (pdf_viewer.prg) *
GOTO EXIT

:COMPILEERRORS
@type comp.log
@type warnings.log
ECHO * Errores de compilacion Harbour *
GOTO EXIT

:LINKERROR
ECHO * Errores de link *
ECHO   Ya se uso la lista COMPLETA de libs de tu build.bat real. Si sigue
ECHO   habiendo unresolved externals, es probable que sea un simbolo de
ECHO   nuestro propio codigo (pdf_hbfunc.c) mal referenciado -- pasame el
ECHO   simbolo exacto.
GOTO EXIT

:EXIT
