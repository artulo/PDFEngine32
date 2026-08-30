/* pdf_raster.c
 *
 * Ver pdf_raster.h.
 */

#include "pdf_raster.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* Un punto NaN/Infinito (CTM degenerado en el content stream -- ver
 * BUG REAL en stroke_segment_thick mas abajo) nunca debe llegar a un
 * cast a int ("(int)(p.x+0.5)", limites de scanline, etc.): eso es
 * comportamiento indefinido en C, y en la practica dispara un DOMAIN
 * error de sqrt() o un crash/loop segun el caso. Filtrar aca, en el
 * unico punto de entrada real de coordenadas de punto flotante al
 * rasterizador. */
static int pdf_point_finite(double x, double y)
{
    /* 1e7 pixels de margen -- muy por encima de cualquier bitmap de
     * pagina real, pero MUY por debajo del punto en que un cuadrado
     * intermedio (x*x en fill_convex_quad/stroke_segment_thick) se
     * acerca al desbordamiento de un double (~1e308) -- un umbral
     * como 1e300 dejaba pasar valores que igual desbordaban mas
     * adelante en un calculo intermedio. */
    return (x >= -1e7 && x <= 1e7 && y >= -1e7 && y <= 1e7);
}

/* --- trazo (stroke): Bresenham, 1px, sin grosor variable ---------------- */

static void pdf_raster_line(pdf_bitmap *bmp, int x0, int y0, int x1, int y1, pdf_color c)
{
    int dx, dy, sx, sy, err, e2;

    dx = abs(x1 - x0);
    dy = -abs(y1 - y0);
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = dx + dy;

    for (;;)
    {
        pdf_bitmap_set_pixel(bmp, x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void pdf_raster_stroke_path(pdf_bitmap *bmp, const pdf_path *path, pdf_color c)
{
    int s, i;

    if (bmp == NULL || path == NULL)
        return;

    for (s = 0; s < path->n_subpaths; s++)
    {
        int start = path->subpath_start[s];
        int len   = path->subpath_len[s];

        if (len < 2)
            continue;

        for (i = 0; i < len - 1; i++)
        {
            pdf_path_point p0 = path->points[start + i];
            pdf_path_point p1 = path->points[start + i + 1];
            if (!pdf_point_finite(p0.x, p0.y) || !pdf_point_finite(p1.x, p1.y))
                continue;
            pdf_raster_line(bmp, (int)(p0.x + 0.5), (int)(p0.y + 0.5),
                                  (int)(p1.x + 0.5), (int)(p1.y + 0.5), c);
        }

        if (path->subpath_closed[s])
        {
            pdf_path_point p0 = path->points[start + len - 1];
            pdf_path_point p1 = path->points[start];
            if (pdf_point_finite(p0.x, p0.y) && pdf_point_finite(p1.x, p1.y))
                pdf_raster_line(bmp, (int)(p0.x + 0.5), (int)(p0.y + 0.5),
                                      (int)(p1.x + 0.5), (int)(p1.y + 0.5), c);
        }
    }
}

/* --- relleno de un cuadrilatero convexo (scanline) -- usado para el
 * trazo con grosor real mas abajo --------------------------------------- */

static void fill_convex_quad(pdf_bitmap *bmp, const double *qx, const double *qy, pdf_color c)
{
    double ymin, ymax;
    int iy0, iy1, y, i;

    ymin = ymax = qy[0];
    for (i = 1; i < 4; i++)
    {
        if (qy[i] < ymin) ymin = qy[i];
        if (qy[i] > ymax) ymax = qy[i];
    }
    iy0 = (int)ymin;
    if (iy0 < 0) iy0 = 0;
    iy1 = (int)(ymax + 1.0);
    if (iy1 > bmp->height) iy1 = bmp->height;

    for (y = iy0; y < iy1; y++)
    {
        double yc = y + 0.5;
        double xmin = 1e18, xmax = -1e18;
        int found = 0, ix0, ix1, x;

        for (i = 0; i < 4; i++)
        {
            double x0 = qx[i], y0 = qy[i];
            double x1 = qx[(i + 1) % 4], y1 = qy[(i + 1) % 4];
            if ((y0 <= yc && y1 > yc) || (y1 <= yc && y0 > yc))
            {
                double t = (yc - y0) / (y1 - y0);
                double xi = x0 + t * (x1 - x0);
                if (xi < xmin) xmin = xi;
                if (xi > xmax) xmax = xi;
                found = 1;
            }
        }
        if (!found) continue;

        ix0 = (int)(xmin + 0.5);
        if (ix0 < 0) ix0 = 0;
        ix1 = (int)(xmax + 0.5);
        if (ix1 > bmp->width) ix1 = bmp->width;
        for (x = ix0; x < ix1; x++)
            pdf_bitmap_set_pixel(bmp, x, y, c);
    }
}

static void stroke_segment_thick(pdf_bitmap *bmp, double x0, double y0,
                                  double x1, double y1, double hw, pdf_color c)
{
    double dx, dy, len, nx, ny, qx[4], qy[4];

    /* BUG REAL ENCONTRADO (Arturo: "se introducen lineas que no
     * corresponden" en D-6025IC1r0.pdf -- el fix de subpath_overflow
     * de pdf_path.c destapo este otro, preexistente: un CTM degenerado
     * en algun punto del content stream (un "cm" casi-singular, comun
     * en el "texto de una sola linea" que exporta AutoCAD) produce
     * coordenadas NaN/Infinito para algunos puntos. sqrt() de un
     * NaN/Infinito dispara el manejador DOMAIN error de la runtime de
     * bcc32, y castear ese Infinito/NaN a int mas abajo (en
     * fill_convex_quad, para el rango de scanlines) es UB -- crash o
     * loop segun el caso. Antes nunca se notaba: con el cupo de
     * subpaths viejo (256, ver pdf_path.h) el path se truncaba ANTES
     * de llegar a estos puntos. Con el cupo mas alto, degradar (no
     * dibujar este segmento) en vez de propagar el NaN/Infinito. */
    if (!pdf_point_finite(x0, y0) || !pdf_point_finite(x1, y1))
        return;

    dx = x1 - x0; dy = y1 - y0;
    len = sqrt(dx * dx + dy * dy);

    if (len < 1e-9) { nx = hw; ny = 0.0; }
    else            { nx = -dy / len * hw; ny = dx / len * hw; }

    qx[0] = x0 + nx; qy[0] = y0 + ny;
    qx[1] = x1 + nx; qy[1] = y1 + ny;
    qx[2] = x1 - nx; qy[2] = y1 - ny;
    qx[3] = x0 - nx; qy[3] = y0 - ny;
    fill_convex_quad(bmp, qx, qy, c);
}

void pdf_raster_stroke_path_w(pdf_bitmap *bmp, const pdf_path *path, pdf_color c, double width_px)
{
    int s, i;
    double hw;

    if (bmp == NULL || path == NULL) return;
    if (width_px <= 1.0) { pdf_raster_stroke_path(bmp, path, c); return; }

    hw = width_px / 2.0;

    for (s = 0; s < path->n_subpaths; s++)
    {
        int start = path->subpath_start[s];
        int len   = path->subpath_len[s];

        /* BUG REAL ENCONTRADO (confirmado contra
         * Utilization_and_efficiency_of_ground_gra.pdf -- un subpath
         * degenerado "moveto X lineto X h S": moveto+lineto AL MISMO
         * PUNTO da len==2, no len<2 -- asi que un guard simple 'len<2'
         * NO alcanza para detectarlo (se probo primero y no arreglo
         * nada, ver historial). Hace falta chequear si TODOS los puntos
         * del subpath coinciden (ningun segmento real, de largo > 0).
         * T.32000-1 8.5.3.2: ese tipo de subpath solo produce marca
         * visible con line cap round/projecting-square -- este motor no
         * distingue estilos de cap (ver limitaciones), asi que la
         * opcion mas segura es NO marcar nada. Sin este chequeo, el
         * bucle de "juntas" (mas abajo) dibujaba un cuadrado
         * width x width en cada punto SIN IMPORTAR que el/los
         * segmento(s) tuvieran largo cero -- con un ancho de linea
         * grande (comun en logos/iconos vectoriales convertidos de SVG,
         * que a veces dejan puntos degenerados como artefacto), esto
         * pintaba un cuadrado NEGRO enorme encima de cualquier cosa que
         * hubiera debajo (confirmado: tapaba media imagen del logo de
         * Elsevier). */
        if (len < 2) continue;
        {
            int has_real_segment = 0;
            for (i = 0; i < len - 1 && !has_real_segment; i++)
            {
                pdf_path_point p0 = path->points[start + i];
                pdf_path_point p1 = path->points[start + i + 1];
                if (p0.x != p1.x || p0.y != p1.y) has_real_segment = 1;
            }
            if (!has_real_segment && path->subpath_closed[s])
            {
                pdf_path_point p0 = path->points[start + len - 1];
                pdf_path_point p1 = path->points[start];
                if (p0.x != p1.x || p0.y != p1.y) has_real_segment = 1;
            }
            if (!has_real_segment) continue;
        }

        for (i = 0; i < len - 1; i++)
        {
            pdf_path_point p0 = path->points[start + i];
            pdf_path_point p1 = path->points[start + i + 1];
            stroke_segment_thick(bmp, p0.x, p0.y, p1.x, p1.y, hw, c);
        }

        if (path->subpath_closed[s])
        {
            pdf_path_point p0 = path->points[start + len - 1];
            pdf_path_point p1 = path->points[start];
            stroke_segment_thick(bmp, p0.x, p0.y, p1.x, p1.y, hw, c);
        }

        /* juntas: un cuadrado del ancho del trazo en cada vertice, para
         * tapar los huecos que dejarian los rectangulos de segmento
         * individuales en las esquinas -- aproximacion simple de join
         * (no distingue miter/round/bevel, pero visualmente aceptable
         * para el grosor tipico de planos tecnicos). Solo se llega aca
         * con len>=2 (ver guard arriba), asi que cada punto SI es
         * vertice de al menos un segmento real. */
        for (i = 0; i < len; i++)
        {
            pdf_path_point p = path->points[start + i];
            double qx[4], qy[4];
            qx[0] = p.x - hw; qy[0] = p.y - hw;
            qx[1] = p.x + hw; qy[1] = p.y - hw;
            qx[2] = p.x + hw; qy[2] = p.y + hw;
            qx[3] = p.x - hw; qy[3] = p.y + hw;
            fill_convex_quad(bmp, qx, qy, c);
        }
    }
}


/* --- stroke con cap/join/dash PDF --------------------------------------- */

static void raster_fill_circle(pdf_bitmap *bmp, double cx, double cy,
                               double r, pdf_color c)
{
    int x0, x1, y0, y1, x, y;
    double dx, dy, rr;

    if (r <= 0.0)
        return;

    x0 = (int)(cx - r - 1.0);
    x1 = (int)(cx + r + 1.0);
    y0 = (int)(cy - r - 1.0);
    y1 = (int)(cy + r + 1.0);

    rr = r * r;
    for (y = y0; y <= y1; y++)
    {
        for (x = x0; x <= x1; x++)
        {
            dx = ((double)x + 0.5) - cx;
            dy = ((double)y + 0.5) - cy;
            if (dx * dx + dy * dy <= rr)
                pdf_bitmap_set_pixel(bmp, x, y, c);
        }
    }
}

static void raster_fill_triangle(pdf_bitmap *bmp,
                                 double ax, double ay,
                                 double bx, double by,
                                 double cx, double cy,
                                 pdf_color c)
{
    double minx, maxx, miny, maxy;
    int x0, x1, y0, y1, x, y;
    double px, py, e1, e2, e3;
    double area;

    area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (fabs(area) < 1e-12)
        return;

    minx = ax; maxx = ax;
    miny = ay; maxy = ay;
    if (bx < minx) minx = bx;
    if (bx > maxx) maxx = bx;
    if (cx < minx) minx = cx;
    if (cx > maxx) maxx = cx;
    if (by < miny) miny = by;
    if (by > maxy) maxy = by;
    if (cy < miny) miny = cy;
    if (cy > maxy) maxy = cy;

    x0 = (int)minx - 1; x1 = (int)maxx + 1;
    y0 = (int)miny - 1; y1 = (int)maxy + 1;

    for (y = y0; y <= y1; y++)
    {
        for (x = x0; x <= x1; x++)
        {
            px = (double)x + 0.5;
            py = (double)y + 0.5;
            e1 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
            e2 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
            e3 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
            if ((e1 >= 0.0 && e2 >= 0.0 && e3 >= 0.0) ||
                (e1 <= 0.0 && e2 <= 0.0 && e3 <= 0.0))
                pdf_bitmap_set_pixel(bmp, x, y, c);
        }
    }
}

static void stroke_join(pdf_bitmap *bmp, double px, double py,
                        double dx0, double dy0, double dx1, double dy1,
                        double hw, int join, double miter, pdf_color c)
{
    double l0, l1, n0x, n0y, n1x, n1y;
    double cross, dot, mx, my, ml;

    if (join == 1)
    {
        raster_fill_circle(bmp, px, py, hw, c);
        return;
    }

    l0 = sqrt(dx0 * dx0 + dy0 * dy0);
    l1 = sqrt(dx1 * dx1 + dy1 * dy1);
    if (l0 < 1e-12 || l1 < 1e-12)
        return;

    dx0 /= l0; dy0 /= l0;
    dx1 /= l1; dy1 /= l1;

    cross = dx0 * dy1 - dy0 * dx1;
    if (fabs(cross) < 1e-12)
        return;

    /* Un poligono de bevel en el lado exterior. */
    n0x = -dy0 * hw; n0y = dx0 * hw;
    n1x = -dy1 * hw; n1y = dx1 * hw;

    if (cross < 0.0)
    {
        n0x = -n0x; n0y = -n0y;
        n1x = -n1x; n1y = -n1y;
    }

    if (join == 2)
    {
        raster_fill_triangle(bmp,
                             px + n0x, py + n0y,
                             px + n1x, py + n1y,
                             px, py, c);
        return;
    }

    /* Miter: interseccion de las dos rectas desplazadas.
     * Si excede el limite, se degrada a bevel. */
    dot = dx0 * n1x + dy0 * n1y;
    if (fabs(dot) < 1e-12)
    {
        raster_fill_triangle(bmp,
                             px + n0x, py + n0y,
                             px + n1x, py + n1y,
                             px, py, c);
        return;
    }

    mx = px + n1x + dx1 * (((px + n0x) - (px + n1x)) * dy0 -
                           ((py + n0y) - (py + n1y)) * dx0) / cross;
    my = py + n1y + dy1 * (((px + n0x) - (px + n1x)) * dy0 -
                           ((py + n0y) - (py + n1y)) * dx0) / cross;
    ml = sqrt((mx - px) * (mx - px) + (my - py) * (my - py));

    if (ml > hw * ((miter > 0.0) ? miter : 10.0))
    {
        raster_fill_triangle(bmp,
                             px + n0x, py + n0y,
                             px + n1x, py + n1y,
                             px, py, c);
    }
    else
    {
        raster_fill_triangle(bmp,
                             px + n0x, py + n0y,
                             mx, my,
                             px + n1x, py + n1y, c);
    }
}

static void stroke_one_segment(pdf_bitmap *bmp, double x0, double y0,
                               double x1, double y1, double hw,
                               int cap_start, int cap_end, pdf_color c)
{
    double dx, dy, len, nx, ny;
    double ax, ay, bx, by;
    double qx[4], qy[4];

    dx = x1 - x0; dy = y1 - y0;
    len = sqrt(dx * dx + dy * dy);
    if (len < 1e-12)
    {
        if (cap_start == 1 || cap_end == 1)
            raster_fill_circle(bmp, x0, y0, hw, c);
        else
            stroke_segment_thick(bmp, x0, y0, x1, y1, hw, c);
        return;
    }

    dx /= len; dy /= len;
    nx = -dy * hw; ny = dx * hw;

    ax = x0; ay = y0; bx = x1; by = y1;

    if (cap_start == 2) { ax -= dx * hw; ay -= dy * hw; }
    if (cap_end   == 2) { bx += dx * hw; by += dy * hw; }

    qx[0] = ax + nx; qy[0] = ay + ny;
    qx[1] = bx + nx; qy[1] = by + ny;
    qx[2] = bx - nx; qy[2] = by - ny;
    qx[3] = ax - nx; qy[3] = ay - ny;
    fill_convex_quad(bmp, qx, qy, c);

    if (cap_start == 1) raster_fill_circle(bmp, x0, y0, hw, c);
    if (cap_end == 1) raster_fill_circle(bmp, x1, y1, hw, c);
}

void pdf_raster_stroke_path_style(pdf_bitmap *bmp, const pdf_path *path,
                                  pdf_color c, double width_px,
                                  int line_cap, int line_join,
                                  double miter_limit,
                                  const double *dash, int dash_count,
                                  double dash_phase)
{
    int s, i, k, on;
    double hw, dx, dy, len, pos, remain, take, local;
    double x0, y0, x1, y1, ax, ay, bx, by;
    double total, phase;
    double *pat;

    if (bmp == NULL || path == NULL || width_px <= 0.0)
        return;

    hw = width_px * 0.5;
    if (line_cap < 0 || line_cap > 2) line_cap = 0;
    if (line_join < 0 || line_join > 2) line_join = 0;
    if (miter_limit <= 0.0) miter_limit = 10.0;

    pat = (double *)malloc((dash_count > 0 ? dash_count : 1) * sizeof(double));
    if (pat == NULL)
        return;

    total = 0.0;
    for (k = 0; k < dash_count; k++)
    {
        pat[k] = (dash[k] >= 0.0) ? dash[k] : 0.0;
        total += pat[k];
    }

    for (s = 0; s < path->n_subpaths; s++)
    {
        int start = path->subpath_start[s];
        int n = path->subpath_len[s];
        int closed = path->subpath_closed[s];

        if (n < 2)
            continue;

        phase = dash_phase;
        k = 0;
        on = 1;
        if (dash_count > 0 && total > 0.0)
        {
            phase = fmod(phase, total);
            if (phase < 0.0) phase += total;
            while (k < dash_count && phase >= pat[k])
            {
                phase -= pat[k];
                k++;
                if (k >= dash_count) k = 0;
                on = !on;
            }
            if (k >= dash_count) k = 0;
        }

        for (i = 0; i < n - 1 + (closed ? 1 : 0); i++)
        {
            pdf_path_point p0 = path->points[start + (i % n)];
            pdf_path_point p1 = path->points[start + ((i + 1) % n)];

            x0 = p0.x; y0 = p0.y; x1 = p1.x; y1 = p1.y;
            dx = x1 - x0; dy = y1 - y0;
            len = sqrt(dx * dx + dy * dy);
            if (len < 1e-12)
                continue;

            pos = 0.0;
            while (pos < len - 1e-12)
            {
                if (dash_count <= 0 || total <= 0.0)
                {
                    stroke_one_segment(bmp, x0, y0, x1, y1, hw,
                                       line_cap, line_cap, c);
                    break;
                }

                remain = pat[k] - phase;
                if (remain <= 1e-12)
                {
                    phase = 0.0;
                    k++;
                    if (k >= dash_count) k = 0;
                    on = !on;
                    continue;
                }

                take = len - pos;
                if (take > remain) take = remain;

                local = pos / len;
                ax = x0 + dx * local;
                ay = y0 + dy * local;
                local = (pos + take) / len;
                bx = x0 + dx * local;
                by = y0 + dy * local;

                if (on)
                    stroke_one_segment(bmp, ax, ay, bx, by, hw, 0, 0, c);

                pos += take;
                phase += take;
                if (phase >= pat[k] - 1e-12)
                {
                    phase = 0.0;
                    k++;
                    if (k >= dash_count) k = 0;
                    on = !on;
                }
            }
        }

        /* Para trazos no discontinuos, aplicar joins entre segmentos. */
        if (dash_count <= 0 || total <= 0.0)
        {
            for (i = 1; i < n - 1; i++)
            {
                pdf_path_point a = path->points[start + i - 1];
                pdf_path_point p = path->points[start + i];
                pdf_path_point b = path->points[start + i + 1];
                stroke_join(bmp, p.x, p.y,
                            p.x - a.x, p.y - a.y,
                            b.x - p.x, b.y - p.y,
                            hw, line_join, miter_limit, c);
            }
            if (closed && n > 2)
            {
                pdf_path_point a = path->points[start + n - 1];
                pdf_path_point p = path->points[start];
                pdf_path_point b = path->points[start + 1];
                stroke_join(bmp, p.x, p.y,
                            p.x - a.x, p.y - a.y,
                            b.x - p.x, b.y - p.y,
                            hw, line_join, miter_limit, c);
            }
        }
    }

    free(pat);
}

/* --- relleno (fill): scanline con nonzero o evenodd --------------------- */

#define PDF_RASTER_MAX_CROSSINGS 256

typedef struct
{
    double x;
    int    winding; /* +1 o -1 segun direccion del borde */
} pdf_xing;

void pdf_raster_fill_path(pdf_bitmap *bmp, const pdf_path *path,
                           pdf_color c, pdf_fill_rule rule)
{
    double bbox_ymin, bbox_ymax;
    int ymin, ymax, y, s, i;

    if (bmp == NULL || path == NULL || path->n_subpaths == 0)
        return;

    bbox_ymin = 1e30;
    bbox_ymax = -1e30;
    for (s = 0; s < path->n_subpaths; s++)
    {
        int start = path->subpath_start[s];
        int len   = path->subpath_len[s];
        for (i = 0; i < len; i++)
        {
            double py = path->points[start + i].y;
            if (py < bbox_ymin) bbox_ymin = py;
            if (py > bbox_ymax) bbox_ymax = py;
        }
    }

    ymin = (int)bbox_ymin;
    if (ymin < 0) ymin = 0;
    ymax = (int)bbox_ymax + 1;
    if (ymax > bmp->height) ymax = bmp->height;

    /* BUG REAL encontrado con Flujograma_Propuesto.pdf: las lineas de
     * las tablas (encabezado/pie) se dibujan como rectangulos rellenos
     * de 0.5pt de alto o ancho -- muy comun como tecnica para lineas
     * finas. El muestreo normal (centro de cada fila de pixeles,
     * row+0.5) puede simplemente NO CAER dentro del rango vertical de
     * una forma mas angosta que 1 pixel, dependiendo de donde caiga
     * exactamente esa franja -- verificado real: un rectangulo con
     * borde y=50.5 desaparecia del todo, mientras que otro con
     * y=20.2 se dibujaba bien, con el mismo alto de 0.5pt, solo por la
     * posicion fraccionaria distinta.
     *
     * Cuando la forma es mas angosta que 1px, se restringe el rango a
     * UNA SOLA fila: la que contiene el centro vertical de la propia
     * forma. Un primer intento dejaba el rango [ymin,ymax) normal
     * (que para una franja a caballo entre dos filas cubre AMBAS) y
     * usaba el mismo punto de muestreo para las dos, rellenando de
     * mas -- una linea que deberia verse de 1px terminaba en 2px de
     * grosor. Restringiendo a una sola fila (la del centro) se
     * garantiza exactamente 1px de grosor sin importar la posicion
     * fraccionaria, resolviendo el "desaparece" original sin
     * introducir un grosor de mas. */
    if (bbox_ymax - bbox_ymin < 1.0)
    {
        int center_row = (int)((bbox_ymin + bbox_ymax) / 2.0);
        if (center_row < 0) center_row = 0;
        if (center_row >= bmp->height) center_row = bmp->height - 1;
        ymin = center_row;
        ymax = center_row + 1;
    }

    for (y = ymin; y < ymax; y++)
    {
        double sy;
        if (bbox_ymax - bbox_ymin < 1.0)
            sy = (bbox_ymin + bbox_ymax) / 2.0; /* forma mas angosta que 1px: usar su propio centro */
        else
            sy = y + 0.5; /* caso normal: centro de la fila de pixeles */
        {
        pdf_xing xs[PDF_RASTER_MAX_CROSSINGS];
        int nx = 0;

        for (s = 0; s < path->n_subpaths; s++)
        {
            int start = path->subpath_start[s];
            int len   = path->subpath_len[s];

            if (len < 2)
                continue;

            for (i = 0; i < len; i++)
            {
                /* cierre implicito SIEMPRE para relleno, sin importar el
                 * flag 'closed' -- asi lo exige el estandar PDF: un path
                 * abierto se rellena como si estuviera cerrado. */
                pdf_path_point p0 = path->points[start + i];
                pdf_path_point p1 = path->points[start + ((i + 1) % len)];
                double y0 = p0.y, y1 = p1.y;
                double x0 = p0.x, x1 = p1.x;
                int wind;
                double x_int;

                if (y0 == y1)
                    continue; /* borde horizontal: no cruza ninguna scanline */

                if (y0 < y1)
                {
                    if (sy < y0 || sy >= y1) continue;
                    wind = 1;
                }
                else
                {
                    if (sy < y1 || sy >= y0) continue;
                    wind = -1;
                }

                x_int = x0 + (sy - y0) * (x1 - x0) / (y1 - y0);

                if (nx < PDF_RASTER_MAX_CROSSINGS)
                {
                    xs[nx].x       = x_int;
                    xs[nx].winding = wind;
                    nx++;
                }
            }
        }

        if (nx < 2)
            continue;

        /* insertion sort por x -- nx tipicamente chico (docenas, no miles) */
        for (i = 1; i < nx; i++)
        {
            pdf_xing key = xs[i];
            int j = i - 1;
            while (j >= 0 && xs[j].x > key.x)
            {
                xs[j + 1] = xs[j];
                j--;
            }
            xs[j + 1] = key;
        }

        if (rule == PDF_FILL_NONZERO)
        {
            int wind_sum = 0;
            for (i = 0; i < nx - 1; i++)
            {
                wind_sum += xs[i].winding;
                if (wind_sum != 0)
                {
                    int xstart = (int)(xs[i].x + 0.5);
                    int xend   = (int)(xs[i + 1].x + 0.5);
                    /* mismo bug que el de altura sub-pixel (ver comentario
                     * mas arriba), pero en el eje X: una franja vertical
                     * mas angosta que 1px (p.ej. una linea de 0.5pt) puede
                     * colapsar a ancho cero al redondear AMBOS bordes al
                     * mismo pixel, segun su posicion fraccionaria exacta
                     * -- verificado real: para la mitad de las posiciones
                     * fraccionarias posibles, xstart==xend aunque el
                     * intervalo real (xs[i+1].x - xs[i].x) sea mayor a
                     * cero. Si hay un intervalo real pero el redondeo lo
                     * colapsa, forzar al menos 1px de ancho. */
                    if (xend <= xstart && xs[i + 1].x > xs[i].x)
                        xend = xstart + 1;
                    pdf_bitmap_fill_rect(bmp, xstart, y, xend, y + 1, c);
                }
            }
        }
        else /* PDF_FILL_EVENODD */
        {
            for (i = 0; i + 1 < nx; i += 2)
            {
                int xstart = (int)(xs[i].x + 0.5);
                int xend   = (int)(xs[i + 1].x + 0.5);
                if (xend <= xstart && xs[i + 1].x > xs[i].x)
                    xend = xstart + 1;
                pdf_bitmap_fill_rect(bmp, xstart, y, xend, y + 1, c);
            }
        }
        }
    }
}

/* --- relleno con antialiasing (glyphs de texto) -------------------------- */

#define PDF_RASTER_AA_SUBSAMPLES 4
#define PDF_RASTER_AA_MAX_WIDTH  2048

/* Suma 'weight' a coverage[] por cada pixel de columna cubierto
 * (parcial o totalmente) por el intervalo real [xa,xb) -- cobertura
 * horizontal EXACTA (no redondeada), acumulada sobre PDF_RASTER_AA_SUBSAMPLES
 * sub-scanlines por fila de pixeles (que aporta el antialiasing
 * vertical) por el llamador. */
static void aa_accumulate_span(double *coverage, int width, int xmin,
                                double xa, double xb, double weight)
{
    int ixa, ixb, x, cx;

    if (xb <= xa) return;
    if (xa < xmin) xa = xmin;
    if (xb > xmin + width) xb = xmin + width;
    if (xb <= xa) return;

    ixa = (int)floor(xa);
    ixb = (int)floor(xb);

    if (ixa == ixb)
    {
        cx = ixa - xmin;
        if (cx >= 0 && cx < width) coverage[cx] += (xb - xa) * weight;
        return;
    }

    cx = ixa - xmin;
    if (cx >= 0 && cx < width) coverage[cx] += ((double)(ixa + 1) - xa) * weight;

    for (x = ixa + 1; x < ixb; x++)
    {
        cx = x - xmin;
        if (cx >= 0 && cx < width) coverage[cx] += weight;
    }

    cx = ixb - xmin;
    if (cx >= 0 && cx < width) coverage[cx] += (xb - (double)ixb) * weight;
}

void pdf_raster_fill_path_aa(pdf_bitmap *bmp, const pdf_path *path,
                              pdf_color c, pdf_fill_rule rule)
{
    double bbox_ymin, bbox_ymax, bbox_xmin, bbox_xmax;
    int ymin, ymax, xmin, xmax, width, y, s, i, sub;
    static double coverage[PDF_RASTER_AA_MAX_WIDTH];

    if (bmp == NULL || path == NULL || path->n_subpaths == 0)
        return;

    bbox_ymin = bbox_xmin = 1e30;
    bbox_ymax = bbox_xmax = -1e30;
    for (s = 0; s < path->n_subpaths; s++)
    {
        int start = path->subpath_start[s];
        int len   = path->subpath_len[s];
        for (i = 0; i < len; i++)
        {
            double px = path->points[start + i].x, py = path->points[start + i].y;
            if (py < bbox_ymin) bbox_ymin = py;
            if (py > bbox_ymax) bbox_ymax = py;
            if (px < bbox_xmin) bbox_xmin = px;
            if (px > bbox_xmax) bbox_xmax = px;
        }
    }

    ymin = (int)bbox_ymin; if (ymin < 0) ymin = 0;
    ymax = (int)bbox_ymax + 1; if (ymax > bmp->height) ymax = bmp->height;
    xmin = (int)bbox_xmin; if (xmin < 0) xmin = 0;
    xmax = (int)bbox_xmax + 1; if (xmax > bmp->width) xmax = bmp->width;

    width = xmax - xmin;
    if (width <= 0) return;
    /* glyph absurdamente grande (zoom extremo): recortar en vez de
     * desbordar el buffer -- tolerante, mismo criterio que el resto
     * del motor (degradar, no crashear). */
    if (width > PDF_RASTER_AA_MAX_WIDTH) width = PDF_RASTER_AA_MAX_WIDTH;

    for (y = ymin; y < ymax; y++)
    {
        int x;

        for (x = 0; x < width; x++) coverage[x] = 0.0;

        for (sub = 0; sub < PDF_RASTER_AA_SUBSAMPLES; sub++)
        {
            double sy = (double)y + ((double)sub + 0.5) / (double)PDF_RASTER_AA_SUBSAMPLES;
            pdf_xing xs[PDF_RASTER_MAX_CROSSINGS];
            int nx = 0;

            for (s = 0; s < path->n_subpaths; s++)
            {
                int start = path->subpath_start[s];
                int len   = path->subpath_len[s];
                if (len < 2) continue;

                for (i = 0; i < len; i++)
                {
                    pdf_path_point p0 = path->points[start + i];
                    pdf_path_point p1 = path->points[start + ((i + 1) % len)];
                    double y0 = p0.y, y1 = p1.y, x0 = p0.x, x1 = p1.x;
                    int wind;
                    double x_int;

                    if (y0 == y1) continue;
                    if (y0 < y1) { if (sy < y0 || sy >= y1) continue; wind = 1; }
                    else         { if (sy < y1 || sy >= y0) continue; wind = -1; }

                    x_int = x0 + (sy - y0) * (x1 - x0) / (y1 - y0);
                    if (nx < PDF_RASTER_MAX_CROSSINGS) { xs[nx].x = x_int; xs[nx].winding = wind; nx++; }
                }
            }

            if (nx < 2) continue;

            for (i = 1; i < nx; i++)
            {
                pdf_xing key = xs[i];
                int j = i - 1;
                while (j >= 0 && xs[j].x > key.x) { xs[j + 1] = xs[j]; j--; }
                xs[j + 1] = key;
            }

            if (rule == PDF_FILL_NONZERO)
            {
                int wind_sum = 0;
                for (i = 0; i < nx - 1; i++)
                {
                    wind_sum += xs[i].winding;
                    if (wind_sum != 0)
                        aa_accumulate_span(coverage, width, xmin, xs[i].x, xs[i + 1].x,
                                            1.0 / (double)PDF_RASTER_AA_SUBSAMPLES);
                }
            }
            else
            {
                for (i = 0; i + 1 < nx; i += 2)
                    aa_accumulate_span(coverage, width, xmin, xs[i].x, xs[i + 1].x,
                                        1.0 / (double)PDF_RASTER_AA_SUBSAMPLES);
            }
        }

        for (x = 0; x < width; x++)
        {
            if (coverage[x] > 0.001)
            {
                double cov = coverage[x];
                if (cov > 1.0) cov = 1.0;
                pdf_bitmap_set_pixel_coverage(bmp, xmin + x, y, c, cov);
            }
        }
    }
}

void pdf_raster_fill_path_shaded(pdf_bitmap *bmp, const pdf_path *path,
                                 pdf_fill_rule rule,
                                 pdf_shading_pixel_fn color_fn, void *user)
{
    double bbox_ymin, bbox_ymax, bbox_xmin, bbox_xmax;
    int ymin, ymax, xmin, xmax, width, y, s, i, sub;
    static double coverage[PDF_RASTER_AA_MAX_WIDTH];

    if (bmp == NULL || path == NULL || path->n_subpaths == 0 || color_fn == NULL)
        return;

    bbox_ymin = bbox_xmin = 1e30;
    bbox_ymax = bbox_xmax = -1e30;
    for (s = 0; s < path->n_subpaths; s++)
    {
        int start = path->subpath_start[s];
        int len   = path->subpath_len[s];
        for (i = 0; i < len; i++)
        {
            double px = path->points[start + i].x, py = path->points[start + i].y;
            if (py < bbox_ymin) bbox_ymin = py;
            if (py > bbox_ymax) bbox_ymax = py;
            if (px < bbox_xmin) bbox_xmin = px;
            if (px > bbox_xmax) bbox_xmax = px;
        }
    }

    ymin = (int)bbox_ymin; if (ymin < 0) ymin = 0;
    ymax = (int)bbox_ymax + 1; if (ymax > bmp->height) ymax = bmp->height;
    xmin = (int)bbox_xmin; if (xmin < 0) xmin = 0;
    xmax = (int)bbox_xmax + 1; if (xmax > bmp->width) xmax = bmp->width;

    width = xmax - xmin;
    if (width <= 0) return;
    if (width > PDF_RASTER_AA_MAX_WIDTH) width = PDF_RASTER_AA_MAX_WIDTH;

    for (y = ymin; y < ymax; y++)
    {
        int x;

        for (x = 0; x < width; x++) coverage[x] = 0.0;

        for (sub = 0; sub < PDF_RASTER_AA_SUBSAMPLES; sub++)
        {
            double sy = (double)y + ((double)sub + 0.5) / (double)PDF_RASTER_AA_SUBSAMPLES;
            pdf_xing xs[PDF_RASTER_MAX_CROSSINGS];
            int nx = 0;

            for (s = 0; s < path->n_subpaths; s++)
            {
                int start = path->subpath_start[s];
                int len   = path->subpath_len[s];
                if (len < 2) continue;

                for (i = 0; i < len; i++)
                {
                    pdf_path_point p0 = path->points[start + i];
                    pdf_path_point p1 = path->points[start + ((i + 1) % len)];
                    double y0 = p0.y, y1 = p1.y, x0 = p0.x, x1 = p1.x;
                    int wind;
                    double x_int;

                    if (y0 == y1) continue;
                    if (y0 < y1) { if (sy < y0 || sy >= y1) continue; wind = 1; }
                    else         { if (sy < y1 || sy >= y0) continue; wind = -1; }

                    x_int = x0 + (sy - y0) * (x1 - x0) / (y1 - y0);
                    if (nx < PDF_RASTER_MAX_CROSSINGS) { xs[nx].x = x_int; xs[nx].winding = wind; nx++; }
                }
            }

            if (nx < 2) continue;

            for (i = 1; i < nx; i++)
            {
                pdf_xing key = xs[i];
                int j = i - 1;
                while (j >= 0 && xs[j].x > key.x) { xs[j + 1] = xs[j]; j--; }
                xs[j + 1] = key;
            }

            if (rule == PDF_FILL_NONZERO)
            {
                int wind_sum = 0;
                for (i = 0; i < nx - 1; i++)
                {
                    wind_sum += xs[i].winding;
                    if (wind_sum != 0)
                        aa_accumulate_span(coverage, width, xmin, xs[i].x, xs[i + 1].x,
                                            1.0 / (double)PDF_RASTER_AA_SUBSAMPLES);
                }
            }
            else
            {
                for (i = 0; i + 1 < nx; i += 2)
                    aa_accumulate_span(coverage, width, xmin, xs[i].x, xs[i + 1].x,
                                        1.0 / (double)PDF_RASTER_AA_SUBSAMPLES);
            }
        }

        for (x = 0; x < width; x++)
        {
            if (coverage[x] > 0.001)
            {
                double cov = coverage[x];
                pdf_color c;
                if (cov > 1.0) cov = 1.0;
                if (color_fn(user, xmin + x, y, &c))
                    pdf_bitmap_set_pixel_coverage(bmp, xmin + x, y, c, cov);
            }
        }
    }
}

/* --- mascara de clip (paths de clip no rectangulares) -------------------- */

/* Detecta si 'path' es exactamente un rectangulo alineado a ejes: un
 * unico subpath de 4 puntos donde cada arista consecutiva (incluido el
 * cierre) es puramente horizontal o vertical. Cubre el caso 're W n'
 * (con o sin rotacion de 90/180/270 grados via CTM, que sigue dando
 * aristas alineadas a ejes en espacio de dispositivo) -- NO cubre un
 * rectangulo rotado a un angulo arbitrario, que cae al camino de
 * mascara real (correcto, aunque mas caro). */
static int path_is_axis_aligned_rect(const pdf_path *path)
{
    int i;
    const double eps = 0.01;

    if (path->n_subpaths != 1)
        return 0;
    if (path->subpath_len[0] != 4)
        return 0;

    {
        int start = path->subpath_start[0];
        for (i = 0; i < 4; i++)
        {
            pdf_path_point p0 = path->points[start + i];
            pdf_path_point p1 = path->points[start + ((i + 1) % 4)];
            double dx = p1.x - p0.x, dy = p1.y - p0.y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx > eps && dy > eps)
                return 0; /* arista diagonal: no es axis-aligned */
        }
    }
    return 1;
}

int pdf_raster_path_is_rect(const pdf_path *path)
{
    if (path == NULL || path->n_subpaths == 0)
        return 0;
    return path_is_axis_aligned_rect(path);
}

int pdf_raster_rasterize_clip_mask(const pdf_path *path, pdf_fill_rule rule,
                                    int bmp_width, int bmp_height,
                                    unsigned char *out_mask)
{
    double bbox_ymin, bbox_ymax, bbox_xmin, bbox_xmax;
    int ymin, ymax, xmin, xmax, width, y, s, i, sub;
    static double coverage[PDF_RASTER_AA_MAX_WIDTH];

    if (path == NULL || out_mask == NULL || bmp_width <= 0 || bmp_height <= 0)
        return 0;
    if (path->n_subpaths == 0)
        return 0;
    if (path_is_axis_aligned_rect(path))
        return 0;

    memset(out_mask, 0, (size_t)bmp_width * (size_t)bmp_height);

    bbox_ymin = bbox_xmin = 1e30;
    bbox_ymax = bbox_xmax = -1e30;
    for (s = 0; s < path->n_subpaths; s++)
    {
        int start = path->subpath_start[s];
        int len   = path->subpath_len[s];
        for (i = 0; i < len; i++)
        {
            double px = path->points[start + i].x, py = path->points[start + i].y;
            if (py < bbox_ymin) bbox_ymin = py;
            if (py > bbox_ymax) bbox_ymax = py;
            if (px < bbox_xmin) bbox_xmin = px;
            if (px > bbox_xmax) bbox_xmax = px;
        }
    }

    ymin = (int)bbox_ymin; if (ymin < 0) ymin = 0;
    ymax = (int)bbox_ymax + 1; if (ymax > bmp_height) ymax = bmp_height;
    xmin = (int)bbox_xmin; if (xmin < 0) xmin = 0;
    xmax = (int)bbox_xmax + 1; if (xmax > bmp_width) xmax = bmp_width;

    width = xmax - xmin;
    if (width <= 0) return 1; /* path fuera del bitmap: mascara ya quedo en 0 (todo clipeado) */
    if (width > PDF_RASTER_AA_MAX_WIDTH) width = PDF_RASTER_AA_MAX_WIDTH;

    for (y = ymin; y < ymax; y++)
    {
        int x;

        for (x = 0; x < width; x++) coverage[x] = 0.0;

        for (sub = 0; sub < PDF_RASTER_AA_SUBSAMPLES; sub++)
        {
            double sy = (double)y + ((double)sub + 0.5) / (double)PDF_RASTER_AA_SUBSAMPLES;
            pdf_xing xs[PDF_RASTER_MAX_CROSSINGS];
            int nx = 0;

            for (s = 0; s < path->n_subpaths; s++)
            {
                int start = path->subpath_start[s];
                int len   = path->subpath_len[s];
                if (len < 2) continue;

                for (i = 0; i < len; i++)
                {
                    pdf_path_point p0 = path->points[start + i];
                    pdf_path_point p1 = path->points[start + ((i + 1) % len)];
                    double y0 = p0.y, y1 = p1.y, x0 = p0.x, x1 = p1.x;
                    int wind;
                    double x_int;

                    if (y0 == y1) continue;
                    if (y0 < y1) { if (sy < y0 || sy >= y1) continue; wind = 1; }
                    else         { if (sy < y1 || sy >= y0) continue; wind = -1; }

                    x_int = x0 + (sy - y0) * (x1 - x0) / (y1 - y0);
                    if (nx < PDF_RASTER_MAX_CROSSINGS) { xs[nx].x = x_int; xs[nx].winding = wind; nx++; }
                }
            }

            if (nx < 2) continue;

            for (i = 1; i < nx; i++)
            {
                pdf_xing key = xs[i];
                int j = i - 1;
                while (j >= 0 && xs[j].x > key.x) { xs[j + 1] = xs[j]; j--; }
                xs[j + 1] = key;
            }

            if (rule == PDF_FILL_NONZERO)
            {
                int wind_sum = 0;
                for (i = 0; i < nx - 1; i++)
                {
                    wind_sum += xs[i].winding;
                    if (wind_sum != 0)
                        aa_accumulate_span(coverage, width, xmin, xs[i].x, xs[i + 1].x,
                                            1.0 / (double)PDF_RASTER_AA_SUBSAMPLES);
                }
            }
            else
            {
                for (i = 0; i + 1 < nx; i += 2)
                    aa_accumulate_span(coverage, width, xmin, xs[i].x, xs[i + 1].x,
                                        1.0 / (double)PDF_RASTER_AA_SUBSAMPLES);
            }
        }

        for (x = 0; x < width; x++)
        {
            if (coverage[x] > 0.001)
            {
                double cov = coverage[x];
                int mv;
                if (cov > 1.0) cov = 1.0;
                mv = (int)(cov * 255.0 + 0.5);
                out_mask[(size_t)y * (size_t)bmp_width + (size_t)(xmin + x)] = (unsigned char)mv;
            }
        }
    }

    return 1;
}
