/* pdf_afm.h
 *
 * Metricas AFM (Adobe Font Metrics) de las 14 fuentes "estandar" de
 * PDF (T.32000-1 9.6.2.2 / Annex D): Times-Roman/Bold/Italic/
 * BoldItalic, Helvetica/-Bold/-Oblique/-BoldOblique,
 * Courier/-Bold/-Oblique/-BoldOblique, Symbol, ZapfDingbats.
 *
 * BUG REAL ENCONTRADO (confirmado contra
 * 2006-ThermodynamicModellingofKIVCETLeadSmeltingProcess-TMS-2.pdf,
 * un PDF academico tipico generado sin embeber fuentes): un font
 * dict "/Type1 /BaseFont /Times-Bold" SIN /Widths y SIN
 * /FontDescriptor es 100% valido segun el estandar -- el lector debe
 * conocer de memoria las metricas de las 14 fuentes estandar (son
 * parte del PDF spec desde 1993, todo software de produccion las
 * trae incorporadas). Sin esto, pdf_font_get_width (ver pdf_font.c)
 * caia al generico 500/1000 em parejo para CADA caracter -- con
 * fuentes reales como Times (donde 'i' es angosta, ~278, y 'm' es
 * ancha, ~778-889) el texto se desviaba acumulativamente linea a
 * linea, MUY visible especificamente en las lineas de texto
 * justificado que el generador del PDF corta en dos comandos TJ con
 * un Td intermedio de reposicionamiento absoluto (comun en salida de
 * TeX/LaTeX): la posicion asumida por ese Td (calculada por el
 * generador con las metricas REALES) no coincidia con el desvio
 * acumulado por este motor (con 500 parejo), y el segundo tramo de
 * texto quedaba superpuesto sobre el final del primero en vez de
 * continuarlo. Antes de esta ronda esto era menos notorio porque el
 * texto se dibujaba como caja placeholder (sin forma real, un
 * desalineo de unos pocos pixels no saltaba tanto a la vista); con
 * contornos reales de glyph el efecto es muy visible.
 *
 * Alcance: solo el rango ASCII imprimible (32-126, identico en
 * StandardEncoding/WinAnsiEncoding/MacRomanEncoding, asi que no
 * depende de que /Encoding declare el PDF) para las 8 variantes
 * Times/Helvetica -- cubre la enorme mayoria del texto en ingles/
 * espanol sin tildes de un documento real. Courier (las 4 variantes)
 * es monoespaciada, 600/1000 em SIEMPRE, sin necesidad de tabla.
 * Symbol/ZapfDingbats no tienen tabla (glyphs no-Latin, uso mucho mas
 * raro) -- pdf_afm_width devuelve -1 para esos dos y el llamador cae
 * al generico 500 igual que antes, sin regresion. */

#ifndef PDF_AFM_H
#define PDF_AFM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Busca 'base_font' (tal cual viene de /BaseFont, p.ej. "Times-Bold"
 * o "Helvetica-BoldOblique") entre las 8 variantes Times/Helvetica
 * con tabla completa. Devuelve el ancho en milesimos de em para
 * 'code' (32-126) si la fuente y el codigo estan cubiertos, o -1 si
 * no (fuente no reconocida como estandar, o codigo fuera del rango
 * ASCII imprimible) -- el llamador (pdf_font_load) debe conservar el
 * fallback generico existente para el caso -1. Reconoce el nombre
 * exacto Y variantes MUY comunes con un sufijo de subconjunto
 * (p.ej. "ABCDEF+Times-Bold") ignorando el prefijo de 6 letras + '+'.
 * Courier (cualquier variante) se detecta aparte por el llamador via
 * pdf_afm_is_courier(), ya que su ancho es constante (no necesita
 * tabla). */
int pdf_afm_width(const char *base_font, int code);

/* Devuelve 1 si 'base_font' es alguna de las 4 variantes Courier
 * (ancho constante 600/1000 em para todo codigo ASCII imprimible),
 * 0 si no. */
int pdf_afm_is_courier(const char *base_font);

#ifdef __cplusplus
}
#endif

#endif /* PDF_AFM_H */
