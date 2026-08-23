Subsistema de texto -- render real de glyphs (ver DESIGN.md, ronda
"render de fuentes real").

Implementado:
  - pdf_font.h / pdf_font.c: parseo de recursos de fuente simple
    (/BaseFont, /FirstChar, /LastChar, /Widths) y compuesta (Type0/CID,
    ver 'is_cid'). Si /FontDescriptor trae /FontFile2 (TrueType
    embebido), lo descomprime y lo parsea (pdf_ttf_load) hacia
    'font->embedded_ttf' -- ver 'has_embedded_ttf'. /CIDToGIDMap
    (stream) tambien se decodifica hacia 'cid_to_gid'.
  - pdf_ttf.h / pdf_ttf.c: parser TrueType (sfnt) propio -- directorio
    de tablas, head/maxp/loca/glyf (glyphs simples y compuestos, con
    subdivision de curvas cuadraticas a segmentos de recta) y cmap
    (formatos 4/12 para unicode, (3,0) "Symbol" para Wingdings/Webdings/
    Symbol). Sin dependencias externas (ni FreeType ni GDI) -- mismo
    estilo C89 minimalista del resto del motor.
  - pdf_ttf_find_system_font / _symbol_font: sustitucion por una fuente
    real de sistema (arial/times/cour + variantes bold/italic, symbol,
    wingding, webding) cuando el PDF no trae /FontFile2 -- lee el
    archivo .ttf DIRECTO del disco (getenv("SystemRoot")+"\Fonts\" +
    fopen/fread, SIN ninguna llamada a GDI/Win32), con una cache de
    vida de proceso (arena+ledger dedicados, ver pdf_ttf.h).
  - pdf_cff.h / pdf_cff.c: parser CFF (Compact Format Font, /FontFile3
    /Subtype /Type1C) propio, con DOS capacidades:
      1) resolucion de NOMBRE de glyph por codigo (Encoding->Charset->
         String INDEX) -- usada por pdf_font.c para completar
         'to_unicode' con ligaduras/simbolos custom que el PDF no
         declara por /Differences ni /ToUnicode (ver comentario de
         archivo en pdf_cff.h).
      2) interprete COMPLETO de charstrings Type 2 (moveto/lineto/
         curveto en todas sus variantes, los 4 operadores de flex,
         hints leidos solo para saltarlos bien, subrutinas locales y
         globales con sesgo de indice) que entrega el CONTORNO real del
         glyph (curvas cubicas) via 'pdf_cff_glyph_outline' -- mismo
         nivel de fidelidad que el TrueType embebido, para fuentes CFF/
         Type1C reales (comun en PDFs de editoriales tipo Elsevier).
         No soporta CFF CID-keyed (ROS/FDArray/FDSelect) ni 'seac'
         (composicion antigua de acentos) -- ver alcance en pdf_cff.h.
  - pdf_font.c: si /FontDescriptor trae /FontFile3, ademas de resolver
    nombres (arriba) parsea el programa CFF completo hacia
    'font->embedded_cff' -- ver 'has_embedded_cff'.
  - pdf_render.c: estado de texto completo (BT/ET/Tf/Td/TD/Tm/T*/
    Tc/Tw/Tz/TL/Ts/Tr/Tj/TJ/'/") con resolucion de fuente por nombre
    desde /Resources, anchos reales desde /Widths. El camino por
    defecto (sin ningun hook registrado) dibuja el CONTORNO REAL de
    cada glyph -- prioridad: TrueType embebido, despues CFF embebido,
    despues sustituto de sistema (ver 'resolve_glyph' y el bloque CFF
    en 'show_text_bytes', pdf_render.c) -- rellenado antialiased via
    pdf_raster_fill_path_aa (nonzero winding + supersampling, ver
    src/graphics/README.txt). Ya NO una caja placeholder salvo como
    fallback final.
  - Gancho pdf_glyph_draw_fn (pdf_render.h): sigue existiendo en el
    codigo (pdf_render_device_set_glyph_hook), pero YA NO esta
    registrado en harbour/pdf_hbfunc.c -- pdf_demo.exe (el binding
    Harbour/FiveWin) tambien usa el rasterizador propio por defecto
    (mismo camino que el motor portable). El hook GDI
    (pdf_gdi_draw_glyph) queda intacto sin usar, por si hiciera falta
    reactivarlo.

Pendiente:
  - Type1 puro (/FontFile, PFA/PFB con charstrings Type 1 clasicas, NO
    Type 2): no soportado -- rarisimo en PDFs modernos (todas las
    herramientas actuales embeben CFF/Type1C via /FontFile3 en su
    lugar). Si /FontFile/2/3 no resuelve nada dibujable, se cae a
    sustitucion por sistema (o a la caja placeholder si tampoco hay
    sustituto resoluble).
  - CFF CID-keyed (CIDFontType0C, ROS/FDArray/FDSelect): fuera de
    alcance, ver pdf_cff.h -- pdf_cff_load falla a proposito para estos.
  - 'seac' (composicion antigua de acentos via endchar con 4
    argumentos, deprecado en fuentes reales modernas): no soportado.
  - Hinting: se usan los contornos "de diseño" sin ajuste a rejilla de
    pixeles.
  - cmap formato 12 (unicode suplementario/CJK completo, TrueType):
    soportado pero best-effort, no verificado a fondo contra casos
    reales.
  - Limite real encontrado (no de este motor -- del PDF en si, ver
    DESIGN.md): un codigo de caracter puede no tener NINGUNA entrada
    recuperable en ningun lado (ni /Differences, ni /ToUnicode, ni el
    Encoding interno de la fuente embebida) -- confirmado contra
    Utilization_and_efficiency_of_ground_gra.pdf (un "Ö" en el nombre
    de un autor). Ni siquiera el interprete CFF completo puede
    resolver ESE caso especifico: la resolucion de GID via Encoding
    falla ANTES de llegar a dibujar cualquier contorno. Se documenta
    como limite de datos del PDF, no del motor.
  - La caja placeholder sigue existiendo como fallback final (fuente
    rara sin match de sistema, glyph fuera de la fuente, o si no hay
    ni siquiera C:\Windows\Fonts real en el entorno de build/ejecucion)
    -- nunca deja texto en blanco ni crashea (salvo el caso de arriba,
    donde ni el codigo de caracter tiene glyph resoluble en absoluto).
