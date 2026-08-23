#ifndef PDF_TRUETYPE_H
#define PDF_TRUETYPE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pdf_tt_table_s
{
    unsigned long tag;
    unsigned long offset;
    unsigned long length;
} pdf_tt_table;

typedef struct pdf_tt_font_s
{
    const unsigned char *data;
    unsigned long size;
    pdf_tt_table tables[128];
    int table_count;
    unsigned short units_per_em;
    unsigned short num_glyphs;
    short index_to_loc_format;
    unsigned short num_hmetrics;
    unsigned long cmap_offset;
    unsigned long cmap_length;
    unsigned long head_offset;
    unsigned long head_length;
    unsigned long hhea_offset;
    unsigned long hhea_length;
    unsigned long hmtx_offset;
    unsigned long hmtx_length;
    unsigned long maxp_offset;
    unsigned long maxp_length;
    unsigned long loca_offset;
    unsigned long loca_length;
    unsigned long glyf_offset;
    unsigned long glyf_length;
} pdf_tt_font;

int pdf_tt_open(pdf_tt_font *font, const unsigned char *data, unsigned long size);
void pdf_tt_close(pdf_tt_font *font);
int pdf_tt_find_table(const pdf_tt_font *font, unsigned long tag,
                      unsigned long *offset, unsigned long *length);
int pdf_tt_get_glyph_id(const pdf_tt_font *font, unsigned long unicode,
                        unsigned short *glyph_id);
int pdf_tt_get_advance(const pdf_tt_font *font, unsigned short glyph_id,
                       unsigned short *advance);
int pdf_tt_get_glyph_bbox(const pdf_tt_font *font, unsigned short glyph_id,
                          short *x_min, short *y_min,
                          short *x_max, short *y_max);

#ifdef __cplusplus
}
#endif

#endif
