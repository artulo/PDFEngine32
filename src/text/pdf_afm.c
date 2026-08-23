/* pdf_afm.c
 *
 * Ver pdf_afm.h. Tablas de metricas AFM publicas de Adobe para las
 * fuentes "estandar" de PDF (identicas en cualquier libreria PDF de
 * produccion -- no son datos inventados ni propietarios, son parte
 * del estandar PDF/PostScript desde 1985).
 */

#include "pdf_afm.h"
#include <string.h>

/* Anchos (milesimos de em) para codigos ASCII 32(espacio)..126('~'),
 * en ese orden -- 95 valores por fuente. Identico entre
 * StandardEncoding/WinAnsiEncoding/MacRomanEncoding en este rango. */

static const short AFM_TIMES_ROMAN[95] = {
    250,333,408,500,500,833,778,180,333,333,500,564,250,333,250,278,
    500,500,500,500,500,500,500,500,500,500,278,278,564,564,564,444,
    921,722,667,667,722,611,556,722,722,333,389,722,611,889,722,722,
    556,722,667,556,611,722,722,944,722,722,611,333,278,333,469,500,
    333,444,500,444,500,444,333,500,500,278,278,500,278,778,500,500,
    500,500,333,389,278,500,500,722,500,500,444,480,200,480,541
};

static const short AFM_TIMES_BOLD[95] = {
    250,333,555,500,500,1000,833,278,333,333,500,570,250,333,250,278,
    500,500,500,500,500,500,500,500,500,500,333,333,570,570,570,500,
    930,722,667,722,722,667,611,778,778,389,500,778,667,944,722,778,
    611,778,722,556,667,722,722,1000,722,722,667,333,278,333,581,500,
    333,500,556,444,556,444,333,500,556,278,333,556,278,833,556,500,
    556,556,444,389,333,556,500,722,500,500,444,394,220,394,520
};

static const short AFM_TIMES_ITALIC[95] = {
    250,333,420,500,500,833,778,214,333,333,500,675,250,333,250,278,
    500,500,500,500,500,500,500,500,500,500,333,333,675,675,675,500,
    920,611,611,667,722,611,611,722,722,333,444,667,556,833,667,722,
    611,722,611,500,556,722,611,833,611,556,556,389,278,389,422,500,
    333,500,500,444,500,444,278,500,500,278,278,444,278,722,500,500,
    500,500,389,389,278,500,444,667,444,444,389,400,275,400,541
};

static const short AFM_TIMES_BOLDITALIC[95] = {
    250,389,555,500,500,833,778,278,333,333,500,570,250,333,250,278,
    500,500,500,500,500,500,500,500,500,500,333,333,570,570,570,500,
    832,667,667,667,722,667,667,722,778,389,500,667,611,889,722,722,
    611,722,667,556,611,722,667,889,667,611,611,333,278,333,570,500,
    333,500,500,444,500,444,333,500,556,278,278,500,278,778,556,500,
    500,500,389,389,278,556,444,667,500,444,389,348,220,348,570
};

static const short AFM_HELVETICA[95] = {
    278,278,355,556,556,889,667,191,333,333,389,584,278,333,278,278,
    556,556,556,556,556,556,556,556,556,556,278,278,584,584,584,556,
    1015,667,667,722,722,667,611,778,722,278,500,667,556,833,722,778,
    667,778,722,667,611,722,667,944,667,667,611,278,278,278,469,556,
    333,556,556,500,556,556,278,556,556,222,222,500,222,833,556,556,
    556,556,333,500,278,556,500,722,500,500,500,334,260,334,584
};

static const short AFM_HELVETICA_BOLD[95] = {
    278,333,474,556,556,889,722,238,333,333,389,584,278,333,278,278,
    556,556,556,556,556,556,556,556,556,556,333,333,584,584,584,611,
    975,722,722,722,722,667,611,778,722,278,556,722,611,833,722,778,
    667,778,722,667,611,722,667,944,667,667,611,333,278,333,584,556,
    333,556,611,556,611,556,333,611,611,278,278,556,278,889,611,611,
    611,611,389,556,333,611,556,778,556,556,500,389,280,389,584
};

/* Helvetica-Oblique / Helvetica-BoldOblique: mismos anchos que su
 * base recta (solo cambia la inclinacion visual, no el avance). */
#define AFM_HELVETICA_OBLIQUE      AFM_HELVETICA
#define AFM_HELVETICA_BOLDOBLIQUE  AFM_HELVETICA_BOLD

typedef struct { const char *name; const short *widths; } afm_entry;

static const afm_entry AFM_TABLE[] = {
    { "Times-Roman",              AFM_TIMES_ROMAN },
    { "Times-Bold",                AFM_TIMES_BOLD },
    { "Times-Italic",              AFM_TIMES_ITALIC },
    { "Times-BoldItalic",          AFM_TIMES_BOLDITALIC },
    { "Helvetica",                 AFM_HELVETICA },
    { "Helvetica-Bold",            AFM_HELVETICA_BOLD },
    { "Helvetica-Oblique",         AFM_HELVETICA_OBLIQUE },
    { "Helvetica-BoldOblique",     AFM_HELVETICA_BOLDOBLIQUE },
    { "Arial",                     AFM_HELVETICA }, /* sustituto MUY comun, metricas practicamente identicas */
    { "Arial,Bold",                AFM_HELVETICA_BOLD },
    { "Arial,Italic",              AFM_HELVETICA_OBLIQUE },
    { "Arial,BoldItalic",          AFM_HELVETICA_BOLDOBLIQUE },
    { "Arial-Bold",                AFM_HELVETICA_BOLD },
    { "Arial-Italic",              AFM_HELVETICA_OBLIQUE },
    { "Arial-BoldItalic",          AFM_HELVETICA_BOLDOBLIQUE },
    { "TimesNewRoman",             AFM_TIMES_ROMAN },
    { "TimesNewRoman,Bold",        AFM_TIMES_BOLD },
    { "TimesNewRoman,Italic",      AFM_TIMES_ITALIC },
    { "TimesNewRoman,BoldItalic",  AFM_TIMES_BOLDITALIC },
    { "TimesNewRomanPSMT",         AFM_TIMES_ROMAN },
    { "TimesNewRomanPS-BoldMT",    AFM_TIMES_BOLD },
    { "TimesNewRomanPS-ItalicMT",  AFM_TIMES_ITALIC },
    { "TimesNewRomanPS-BoldItalicMT", AFM_TIMES_BOLDITALIC }
};
#define AFM_TABLE_COUNT (sizeof(AFM_TABLE) / sizeof(AFM_TABLE[0]))

/* /BaseFont puede traer un prefijo de subconjunto "ABCDEF+" (6
 * letras mayusculas + '+', ver T.32000-1 9.6.4.3) -- se lo saltea
 * antes de comparar, mismo criterio que el resto del motor. */
static const char *skip_subset_prefix(const char *name)
{
    int i;
    if (name == NULL) return name;
    for (i = 0; i < 6; i++)
        if (name[i] < 'A' || name[i] > 'Z') return name;
    if (name[6] == '+') return name + 7;
    return name;
}

int pdf_afm_width(const char *base_font, int code)
{
    const char *name;
    size_t i;

    if (base_font == NULL || code < 32 || code > 126)
        return -1;

    name = skip_subset_prefix(base_font);
    for (i = 0; i < AFM_TABLE_COUNT; i++)
        if (strcmp(name, AFM_TABLE[i].name) == 0)
            return (int)AFM_TABLE[i].widths[code - 32];

    return -1;
}

int pdf_afm_is_courier(const char *base_font)
{
    const char *name = skip_subset_prefix(base_font);
    if (name == NULL) return 0;
    return (strncmp(name, "Courier", 7) == 0) ? 1 : 0;
}
