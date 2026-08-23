#include "pdf_truetype.h"
#include <string.h>

static unsigned short tt_u16(const unsigned char *p)
{
    return (unsigned short)(((unsigned short)p[0] << 8) | p[1]);
}

static short tt_s16(const unsigned char *p)
{
    return (short)tt_u16(p);
}

static unsigned long tt_u32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) |
           ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) |
           (unsigned long)p[3];
}

static unsigned long tt_tag(char a, char b, char c, char d)
{
    return ((unsigned long)(unsigned char)a << 24) |
           ((unsigned long)(unsigned char)b << 16) |
           ((unsigned long)(unsigned char)c << 8) |
           (unsigned long)(unsigned char)d;
}

static int tt_ok(const pdf_tt_font *f, unsigned long off, unsigned long len)
{
    if (f == NULL || f->data == NULL) return 0;
    if (off > f->size) return 0;
    if (len > f->size - off) return 0;
    return 1;
}

int pdf_tt_find_table(const pdf_tt_font *f, unsigned long tag,
                      unsigned long *offset, unsigned long *length)
{
    int i;
    if (f == NULL) return 0;
    for (i = 0; i < f->table_count; i++)
    {
        if (f->tables[i].tag == tag)
        {
            if (offset != NULL) *offset = f->tables[i].offset;
            if (length != NULL) *length = f->tables[i].length;
            return 1;
        }
    }
    return 0;
}

int pdf_tt_open(pdf_tt_font *f, const unsigned char *data, unsigned long size)
{
    unsigned short nt;
    unsigned long p;
    int i;

    if (f == NULL || data == NULL || size < 12UL) return 0;
    memset(f, 0, sizeof(*f));
    f->data = data;
    f->size = size;
    nt = tt_u16(data + 4);
    if (nt == 0 || nt > 128) return 0;
    if (12UL + (unsigned long)nt * 16UL > size) return 0;
    p = 12UL;
    f->table_count = (int)nt;

    for (i = 0; i < f->table_count; i++)
    {
        f->tables[i].tag = tt_u32(data + p);
        f->tables[i].offset = tt_u32(data + p + 8UL);
        f->tables[i].length = tt_u32(data + p + 12UL);
        if (!tt_ok(f, f->tables[i].offset, f->tables[i].length)) return 0;
        p += 16UL;
    }

    if (!pdf_tt_find_table(f, tt_tag('h','e','a','d'), &f->head_offset, &f->head_length)) return 0;
    if (!pdf_tt_find_table(f, tt_tag('h','h','e','a'), &f->hhea_offset, &f->hhea_length)) return 0;
    if (!pdf_tt_find_table(f, tt_tag('m','a','x','p'), &f->maxp_offset, &f->maxp_length)) return 0;
    if (!pdf_tt_find_table(f, tt_tag('h','m','t','x'), &f->hmtx_offset, &f->hmtx_length)) return 0;
    if (!pdf_tt_find_table(f, tt_tag('l','o','c','a'), &f->loca_offset, &f->loca_length)) return 0;
    if (!pdf_tt_find_table(f, tt_tag('g','l','y','f'), &f->glyf_offset, &f->glyf_length)) return 0;
    pdf_tt_find_table(f, tt_tag('c','m','a','p'), &f->cmap_offset, &f->cmap_length);

    if (!tt_ok(f, f->head_offset, 54UL) ||
        !tt_ok(f, f->hhea_offset, 36UL) ||
        !tt_ok(f, f->maxp_offset, 6UL)) return 0;

    f->units_per_em = tt_u16(data + f->head_offset + 18UL);
    f->index_to_loc_format = tt_s16(data + f->head_offset + 50UL);
    f->num_glyphs = tt_u16(data + f->maxp_offset + 4UL);
    f->num_hmetrics = tt_u16(data + f->hhea_offset + 34UL);

    if (f->units_per_em == 0 || f->num_glyphs == 0 || f->num_hmetrics == 0) return 0;
    if (f->index_to_loc_format != 0 && f->index_to_loc_format != 1) return 0;
    return 1;
}

void pdf_tt_close(pdf_tt_font *f)
{
    if (f != NULL) memset(f, 0, sizeof(*f));
}

static int tt_cmap4(const pdf_tt_font *f, unsigned long off, unsigned long len,
                    unsigned long unicode, unsigned short *gid)
{
    const unsigned char *p;
    unsigned short seg_count;
    unsigned long end_off, start_off, delta_off, range_off;
    unsigned short i;
    unsigned short c;
    unsigned short ro;
    long g;

    if (!tt_ok(f, off, len) || len < 16UL) return 0;
    p = f->data + off;
    if (tt_u16(p) != 4) return 0;
    if ((unsigned long)tt_u16(p + 2) > len || tt_u16(p + 2) < 16U) return 0;
    seg_count = (unsigned short)(tt_u16(p + 6) / 2U);
    if (seg_count == 0 || seg_count > 256) return 0;
    end_off = 14UL;
    start_off = end_off + (unsigned long)seg_count * 2UL + 2UL;
    delta_off = start_off + (unsigned long)seg_count * 2UL;
    range_off = delta_off + (unsigned long)seg_count * 2UL;
    if (range_off + (unsigned long)seg_count * 2UL > len) return 0;
    if (unicode > 0xFFFFUL) return 0;
    c = (unsigned short)unicode;

    for (i = 0; i < seg_count; i++)
    {
        unsigned short endc = tt_u16(p + end_off + (unsigned long)i * 2UL);
        if (c <= endc)
        {
            unsigned short startc = tt_u16(p + start_off + (unsigned long)i * 2UL);
            if (c < startc) return 0;
            ro = tt_u16(p + range_off + (unsigned long)i * 2UL);
            if (ro == 0)
            {
                short d = tt_s16(p + delta_off + (unsigned long)i * 2UL);
                *gid = (unsigned short)((c + d) & 0xFFFF);
                return 1;
            }
            else
            {
                unsigned long ro_pos = range_off + (unsigned long)i * 2UL;
                unsigned long glyph_pos = ro_pos + ro + (unsigned long)(c - startc) * 2UL;
                if (glyph_pos + 2UL > len) return 0;
                g = (long)tt_u16(p + glyph_pos);
                if (g != 0)
                {
                    short d = tt_s16(p + delta_off + (unsigned long)i * 2UL);
                    g = (g + d) & 0xFFFFL;
                }
                *gid = (unsigned short)g;
                return 1;
            }
        }
    }
    return 0;
}

static int tt_cmap12(const pdf_tt_font *f, unsigned long off, unsigned long len,
                     unsigned long unicode, unsigned short *gid)
{
    const unsigned char *p;
    unsigned long n;
    unsigned long i;
    if (!tt_ok(f, off, len) || len < 16UL) return 0;
    p = f->data + off;
    if (tt_u16(p) != 12) return 0;
    n = tt_u32(p + 12UL);
    if (16UL + n * 12UL > len) return 0;
    for (i = 0; i < n; i++)
    {
        const unsigned char *g = p + 16UL + i * 12UL;
        unsigned long start = tt_u32(g);
        unsigned long end = tt_u32(g + 4UL);
        unsigned long start_gid = tt_u32(g + 8UL);
        if (unicode >= start && unicode <= end)
        {
            unsigned long value = start_gid + unicode - start;
            if (value > 0xFFFFUL) return 0;
            *gid = (unsigned short)value;
            return 1;
        }
    }
    return 0;
}

int pdf_tt_get_glyph_id(const pdf_tt_font *f, unsigned long unicode,
                        unsigned short *glyph_id)
{
    const unsigned char *p;
    unsigned short version, count, i;
    int best4, best12;
    unsigned long best4off, best4len, best12off, best12len;
    unsigned short platform, encoding, format;

    if (f == NULL || glyph_id == NULL || f->cmap_offset == 0) return 0;
    if (!tt_ok(f, f->cmap_offset, f->cmap_length) || f->cmap_length < 4UL) return 0;
    p = f->data + f->cmap_offset;
    version = tt_u16(p);
    count = tt_u16(p + 2UL);
    if (version != 0 || 4UL + (unsigned long)count * 8UL > f->cmap_length) return 0;

    best4 = 0; best12 = 0; best4off = best4len = best12off = best12len = 0;
    for (i = 0; i < count; i++)
    {
        const unsigned char *r = p + 4UL + (unsigned long)i * 8UL;
        platform = tt_u16(r);
        encoding = tt_u16(r + 2UL);
        {
            unsigned long suboff = tt_u32(r + 4UL);
            if (suboff + 2UL > f->cmap_length) continue;
            format = tt_u16(p + suboff);
            if (format == 12 && unicode > 0xFFFFUL && (platform == 3 || platform == 0))
            {
                best12 = 1;
                best12off = f->cmap_offset + suboff;
                best12len = f->cmap_length - suboff;
            }
            else if (format == 12 && (platform == 3 || platform == 0) && !best12)
            {
                best12 = 1;
                best12off = f->cmap_offset + suboff;
                best12len = f->cmap_length - suboff;
            }
            else if (format == 4 && (platform == 3 || platform == 0) && !best4)
            {
                best4 = 1;
                best4off = f->cmap_offset + suboff;
                best4len = f->cmap_length - suboff;
            }
            else if (format == 4 && platform == 1 && !best4)
            {
                best4 = 1;
                best4off = f->cmap_offset + suboff;
                best4len = f->cmap_length - suboff;
            }
            (void)encoding;
        }
    }

    if (best12 && tt_cmap12(f, best12off, best12len, unicode, glyph_id)) return 1;
    if (best4 && tt_cmap4(f, best4off, best4len, unicode, glyph_id)) return 1;
    return 0;
}

int pdf_tt_get_advance(const pdf_tt_font *f, unsigned short glyph_id,
                       unsigned short *advance)
{
    unsigned short mi;
    unsigned long pos;
    if (f == NULL || advance == NULL || glyph_id >= f->num_glyphs) return 0;
    mi = glyph_id < f->num_hmetrics ? glyph_id : (unsigned short)(f->num_hmetrics - 1U);
    pos = f->hmtx_offset + (unsigned long)mi * 4UL;
    if (!tt_ok(f, pos, 4UL)) return 0;
    *advance = tt_u16(f->data + pos);
    return 1;
}

int pdf_tt_get_glyph_bbox(const pdf_tt_font *f, unsigned short glyph_id,
                          short *x_min, short *y_min, short *x_max, short *y_max)
{
    unsigned long a, b, off, next, size;
    if (f == NULL || glyph_id >= f->num_glyphs) return 0;
    if (f->index_to_loc_format == 0)
    {
        a = f->loca_offset + (unsigned long)glyph_id * 2UL;
        b = a + 2UL;
        if (!tt_ok(f, a, 2UL) || !tt_ok(f, b, 2UL)) return 0;
        off = (unsigned long)tt_u16(f->data + a) * 2UL;
        next = (unsigned long)tt_u16(f->data + b) * 2UL;
    }
    else
    {
        a = f->loca_offset + (unsigned long)glyph_id * 4UL;
        b = a + 4UL;
        if (!tt_ok(f, a, 4UL) || !tt_ok(f, b, 4UL)) return 0;
        off = tt_u32(f->data + a);
        next = tt_u32(f->data + b);
    }
    if (next < off) return 0;
    size = next - off;
    if (size == 0UL)
    {
        if (x_min) *x_min = 0;
        if (y_min) *y_min = 0;
        if (x_max) *x_max = 0;
        if (y_max) *y_max = 0;
        return 1;
    }
    if (size < 10UL || off > f->glyf_length || size > f->glyf_length - off) return 0;
    off += f->glyf_offset;
    if (x_min) *x_min = tt_s16(f->data + off + 2UL);
    if (y_min) *y_min = tt_s16(f->data + off + 4UL);
    if (x_max) *x_max = tt_s16(f->data + off + 6UL);
    if (y_max) *y_max = tt_s16(f->data + off + 8UL);
    return 1;
}
