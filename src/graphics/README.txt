Rasterizador de graficos vectoriales -- YA NO esta vacia (ver DESIGN.md
seccion 10).

Implementado:
  - pdf_path.h / pdf_path.c     : construccion de paths (m/l/c/v/y/h/re),
                                   curvas Bezier aplanadas a segmentos.
  - pdf_raster.h / pdf_raster.c : trazo (Bresenham) y relleno (scanline,
                                   nonzero/evenodd, multiples subpaths).

Pendiente (limitaciones conocidas, ver comentarios en pdf_raster.h):
  - Grosor de linea variable (el operador 'w' del content stream se
    ignora, el trazo siempre es de 1px).
  - Clipping (W/W*) -- se ignora, no recorta.
  - Antialiasing.
  - Patrones y transparencias (grupos SMask, /CA /ca).
  - Join/cap de trazos (siempre el que sale de Bresenham).

Validado contra 4 PDFs reales de ingenieria (planos de horno Ausmelt,
diagramas de proceso) con render completo -- ver DESIGN.md seccion 10.
