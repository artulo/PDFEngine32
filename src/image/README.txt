Subsistema de imagenes -- completo, incluido CCITT G4.

Implementado y VERIFICADO (comparado byte a byte contra libtiff real):
  - pdf_image.h / pdf_image.c: resuelve el operador 'Do', decodifica
    XObjects /Subtype /Image (DeviceGray/RGB/CMYK, Indexed con base
    RGB/Gray, ImageMask), y los compone sobre pdf_bitmap con muestreo
    por transformacion inversa (soporta rotacion/shear del CTM) y box
    filter (promedio de area) cuando la imagen se reduce mucho.
  - DCTDecode (JPEG baseline): decodificador propio completo,
    verificado contra JPEGs reales de PDFs de produccion.
  - FlateDecode, ASCII85Decode, y cadenas de filtros (/Filter como
    array, p.ej. [/FlateDecode /DCTDecode]).
  - CCITTFaxDecode (Group 4): reescrito desde cero replicando la
    estructura de libtiff (tif_fax3.h), con tablas T.4 verificadas
    programaticamente contra tif_fax3sm.c (0 diferencias) y el
    algoritmo verificado pixel a pixel contra libtiff real en
    multiples casos (rectangulos, patrones complejos, ruido aleatorio,
    anchos no multiplo de 8, y un PDF real de 111MB con un libro
    escaneado). Ver DESIGN.md seccion 23 para el detalle completo de
    los 4 bugs reales encontrados y como se diagnosticaron.

Pendiente:
  - Form XObjects: implementado (ver pdf_render.c).
  - SMask/transparencia real: se ignora (alpha no soportado).
  - Imagenes inline (BI...ID...EI): pdf_content.c ya las reconoce y
    entrega dict + bytes crudos via pdf_content_inline_image_fn, pero
    todavia no hay ningun consumidor conectado a pdf_render.c.
  - Decodificacion en mosaico (tiles) para imagenes muy grandes: el
    framework de arenas lo soporta, pero no esta implementado -- las
    imagenes se decodifican enteras (el presupuesto de memoria del
    documento debe ser lo bastante grande, ver DESIGN.md seccion 15).
  - JPXDecode (JPEG2000): IMPLEMENTADO, CONECTADO Y FUNCIONAL
    (src/filters/pdf_jpx.c, ~1950 lineas -- MQ decoder, EBCOT
    Tier-1/Tier-2, IDWT 5/3 y 9/7, driver JP2/J2K -- llamado desde
    pdf_image.c en la rama /JPXDecode). Verificado bit-exacto contra
    Pillow/OpenJPEG 2.5.4 en varios casos sinteticos (gradientes,
    imagenes planas, un unico pixel no-cero, con y sin wavelet
    multi-nivel) y visualmente correcto contra la imagen real de
    Conveyor_Handbook.pdf (logo "FENNER DUNLOP").

    Investigado a fondo en TRES rondas (las dos primeras documentadas
    en DESIGN.md secciones 60-60.7 no encontraron la causa raiz real;
    la tercera si, usando un decodificador de referencia real
    instalado en la maquina -- Python+Pillow+OpenJPEG via winget/pip,
    y las fuentes de OpenJPEG 2.5.4 descargadas directo de GitHub para
    comparar linea por linea):

    BUG RAIZ REAL #1 (el que causaba el ruido tipo "tablero de
    ajedrez"): inicializacion de contexto MQ incorrecta en Tier-1
    (cb_state_create). El estandar T.800 (Tabla D.7) y el codigo
    fuente real de OpenJPEG (mqc.c: opj_mqc_reset_enc / t1.c:
    t1_decode_cblk) inicializan TRES contextos con un estado distinto
    de 0, no solo el contexto run-length:
      - ZC_CTX_BASE+0 (el contexto "sin vecinos significativos", el
        mas usado de lejos en la pasada de limpieza) -> estado 4.
      - RL_CTX (run-length) -> estado 3.
      - UNIFORM_CTX -> estado 46.
    La version anterior dejaba los 19 contextos en el estado 0 por
    igual. Encontrado construyendo casos de prueba JPEG2000 sinteticos
    minimos (un unico pixel no-cero en 5/3 sin perdida, comparado bit
    a bit contra la referencia real) y confirmado linea por linea
    contra el codigo fuente de OpenJPEG. Con los 19 contextos en
    estado 0, el decodificador se desincronizaba inmediatamente
    despues del primer coeficiente significativo real de cada
    code-block (coincidencia: el primerito solia salir bien, todo lo
    posterior era ruido) -- exactamente el patron de "checkerboard
    cada vez peor con cada nivel/subbanda" documentado en las rondas
    1 y 2, porque estos 3 contextos son justamente los que usa la
    pasada de limpieza en CADA bitplane.

    BUG RAIZ REAL #2 (amplitud/rango atenuados en la sintesis 9/7,
    solo visible una vez arreglado el #1): el factor de escala K/(1/K)
    del paso 5 de la sintesis 9/7 irreversible (idwt_97_1d) estaba
    invertido -- el lado LOW/par se escalaba con 1/K y el HIGH/impar
    con K, al reves de lo que hace OpenJPEG (dwt.c,
    opj_v8dwt_decode: 'wavelet+a' -- LOW -- con K, 'wavelet+b' --
    HIGH -- con 1/K). Con Tier-1 ya corregido, esto se manifestaba
    como una reconstruccion con la forma/direccion CORRECTA pero el
    rango comprimido a una fraccion chica de lo real (confirmado con
    un gradiente sintetico: forma bien, amplitud ~3% de la real).

    Con AMBOS bugs corregidos: los tres casos sinteticos (gradiente
    16x8 lossless 5/3, imagen plana 4x4, imagen 8x8 con un unico pixel
    no-cero) decodifican BIT-EXACTOS contra la referencia; la imagen
    real del logo de Conveyor_Handbook.pdf decodifica visualmente
    identica a la referencia (diferencia media de pixel bajo de
    268/canal a menos de 3/canal tras el segundo fix).

    Se probaron y DESCARTARON en rondas anteriores (documentado por si
    hace falta revisar de nuevo en el futuro, ninguno era la causa):
    inversion global de signo de coeficiente, intercambio de bandas
    HL<->LH, orden fila-primero vs columna-primero en la sintesis 2D,
    formula de paso de cuantizacion 'mb' vs 'Rb' (este SI era un bug
    real y separado, ya corregido, pero no la causa del ruido).

    Instrumentacion de diagnostico que se mantiene en el codigo
    (variables de entorno, ver pdf_jpx.c): test_jpx.c (decodificador
    standalone -- reconstruir compilando junto a pdf_mem.c+pdf_jpx.c),
    PDF_JPX_DEBUG=1 (traza de paquetes/code-blocks/signos),
    PDF_JPX_DUMP_LL=<ruta> (vuelca LL0 y cada nivel de sintesis como
    PGM), PDF_JPX_NO_RL=1 (desactiva el atajo run-length, ya
    descartado como causa pero se deja para diagnostico futuro).
  - Encriptacion: RC4 clasico (V1/V2, R2/R3), AES-128 (V4/R4/AESV2,
    verificado con un PDF real) y AES-256 (V5/R5-R6/AESV3, primitivos
    verificados pero sin PDF real de prueba) -- ver DESIGN.md seccion
    71 y pdf_crypt.h.
