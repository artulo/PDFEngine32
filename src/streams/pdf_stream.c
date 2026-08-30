/* pdf_stream.c
 *
 * Ver pdf_stream.h.
 */

#include "pdf_stream.h"
#include <string.h>

static void st_set_buf_window(pdf_stream *st, long start, int len, int pos)
{
    memcpy(&st->buf_start, &start, sizeof(start));
    memcpy(&st->buf_len, &len, sizeof(len));
    memcpy(&st->buf_pos, &pos, sizeof(pos));
}

static void st_set_mem(pdf_stream *st, const unsigned char *data, long len, long pos)
{
    memcpy(&st->mem_data, &data, sizeof(data));
    memcpy(&st->mem_len, &len, sizeof(len));
    memcpy(&st->mem_pos, &pos, sizeof(pos));
}

int pdf_stream_open(pdf_stream *st, const char *path)
{
    FILE *fp;
    long size;

    if (st == NULL || path == NULL)
        return PDF_ERR_BADARG;

    fp = fopen(path, "rb");
    if (fp == NULL)
        return PDF_ERR_BADARG;

    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return PDF_ERR_BADARG;
    }
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    memcpy(&st->fp, &fp, sizeof(fp));
    memcpy(&st->file_size, &size, sizeof(size));

    st_set_buf_window(st, 0, 0, 0);
    st_set_mem(st, NULL, 0, 0);

    return PDF_OK;
}

void pdf_stream_open_memory(pdf_stream *st, const unsigned char *data, long len)
{
    FILE *null_fp = NULL;

    if (st == NULL) return;

    memcpy(&st->fp, &null_fp, sizeof(null_fp));
    memcpy(&st->file_size, &len, sizeof(len));
    st_set_buf_window(st, 0, 0, 0);
    st_set_mem(st, data, len, 0);
}

void pdf_stream_close(pdf_stream *st)
{
    if (st == NULL || st->fp == NULL)
        return; /* tambien cubre el modo memoria (fp siempre NULL ahi) */
    fclose(st->fp);
    st->fp = NULL;
}

long pdf_stream_tell(const pdf_stream *st)
{
    if (st == NULL)
        return -1;
    if (st->mem_data != NULL)
        return st->mem_pos;
    return st->buf_start + st->buf_pos;
}

long pdf_stream_size(const pdf_stream *st)
{
    if (st == NULL)
        return 0;
    return st->file_size; /* seteado igual en ambos modos */
}

static int pdf_stream_refill(pdf_stream *st)
{
    long want_start;
    size_t got;

    if (st == NULL || st->fp == NULL)
        return 0;

    want_start = st->buf_start + st->buf_pos;
    if (want_start < 0 || want_start >= st->file_size)
    {
        st_set_buf_window(st, want_start, 0, 0);
        return 0; /* EOF */
    }

    if (fseek(st->fp, want_start, SEEK_SET) != 0)
        return 0;

    got = fread(st->buf, 1, (size_t)PDF_STREAM_BUFSZ, st->fp);

    st_set_buf_window(st, want_start, (int)got, 0);

    return (got > 0);
}

int pdf_stream_seek(pdf_stream *st, long offset)
{
    if (st == NULL || offset < 0)
        return PDF_ERR_BADARG;

    if (st->mem_data != NULL)
    {
        long new_pos = (offset > st->mem_len) ? st->mem_len : offset;
        memcpy(&st->mem_pos, &new_pos, sizeof(new_pos));
        return PDF_OK;
    }

    if (offset >= st->buf_start && offset < st->buf_start + st->buf_len)
    {
        /* cae dentro del buffer actual: no hace falta tocar el disco */
        int new_pos = (int)(offset - st->buf_start);
        memcpy(&st->buf_pos, &new_pos, sizeof(new_pos));
        return PDF_OK;
    }

    /* fuera del buffer: se recarga perezosamente en la proxima lectura */
    st_set_buf_window(st, offset, 0, 0);

    return PDF_OK;
}

int pdf_stream_getc(pdf_stream *st)
{
    int c;

    if (st == NULL)
        return -1;

    if (st->mem_data != NULL)
    {
        if (st->mem_pos >= st->mem_len)
            return -1;
        return st->mem_data[st->mem_pos++];
    }

    if (st->buf_pos >= st->buf_len)
    {
        if (!pdf_stream_refill(st))
            return -1;
    }

    c = st->buf[st->buf_pos];
    st->buf_pos++;
    return c;
}

int pdf_stream_peekc(pdf_stream *st)
{
    int c;
    long save_start;
    int save_len, save_pos;

    if (st == NULL)
        return -1;

    if (st->mem_data != NULL)
    {
        if (st->mem_pos >= st->mem_len)
            return -1;
        return st->mem_data[st->mem_pos];
    }

    save_start = st->buf_start;
    save_len   = st->buf_len;
    save_pos   = st->buf_pos;

    c = pdf_stream_getc(st);

    /* deshacer el avance -- barato porque no volvemos a tocar el disco
     * salvo que getc haya tenido que refill (en cuyo caso restauramos
     * el estado del buffer previo al refill via seek logico). */
    if (c != -1)
    {
        st_set_buf_window(st, save_start, save_len, save_pos);
    }

    return c;
}

long pdf_stream_read(pdf_stream *st, unsigned char *dst, long n)
{
    long total;

    if (st == NULL || dst == NULL || n <= 0)
        return 0;

    if (st->mem_data != NULL)
    {
        long avail = st->mem_len - st->mem_pos;
        long take = (n < avail) ? n : avail;
        if (take < 0) take = 0;
        if (take > 0)
        {
            memcpy(dst, st->mem_data + st->mem_pos, (size_t)take);
            st->mem_pos += take;
        }
        return take;
    }

    total = 0;
    while (total < n)
    {
        long avail;
        long take;

        if (st->buf_pos >= st->buf_len)
        {
            if (!pdf_stream_refill(st))
                break; /* EOF */
        }

        avail = st->buf_len - st->buf_pos;
        take  = n - total;
        if (take > avail)
            take = avail;

        memcpy(dst + total, st->buf + st->buf_pos, (size_t)take);
        st->buf_pos += (int)take;
        total += take;
    }

    return total;
}
