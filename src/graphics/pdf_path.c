/* pdf_path.c
 *
 * Ver pdf_path.h.
 */

#include "pdf_path.h"
#include <stddef.h>

void pdf_path_reset(pdf_path *path)
{
    if (path == NULL) return;
    path->n_points    = 0;
    path->n_subpaths  = 0;
    path->has_current = 0;
}

static void pdf_path_begin_subpath(pdf_path *path, double x, double y)
{
    if (path->n_subpaths >= PDF_PATH_MAX_SUBPATHS)
        return; /* capacidad agotada: se descarta silenciosamente */
    if (path->n_points >= PDF_PATH_MAX_POINTS)
        return;

    path->subpath_start[path->n_subpaths]  = path->n_points;
    path->subpath_len[path->n_subpaths]    = 0;
    path->subpath_closed[path->n_subpaths] = 0;
    path->n_subpaths++;

    path->points[path->n_points].x = x;
    path->points[path->n_points].y = y;
    path->n_points++;
    path->subpath_len[path->n_subpaths - 1] = 1;

    path->cur_x = path->start_x = x;
    path->cur_y = path->start_y = y;
    path->has_current = 1;
}

static void pdf_path_add_point(pdf_path *path, double x, double y)
{
    if (path->n_subpaths == 0)
    {
        /* 'l'/'c' sin un 'm' previo: tratar como moveto implicito (PDF
         * real casi nunca hace esto, pero mejor no crashear). */
        pdf_path_begin_subpath(path, x, y);
        return;
    }
    if (path->n_points >= PDF_PATH_MAX_POINTS)
        return; /* capacidad agotada: se descarta silenciosamente */

    path->points[path->n_points].x = x;
    path->points[path->n_points].y = y;
    path->n_points++;
    path->subpath_len[path->n_subpaths - 1]++;

    path->cur_x = x;
    path->cur_y = y;
    path->has_current = 1;
}

void pdf_path_moveto(pdf_path *path, double x, double y)
{
    if (path == NULL) return;
    pdf_path_begin_subpath(path, x, y);
}

void pdf_path_lineto(pdf_path *path, double x, double y)
{
    if (path == NULL) return;
    pdf_path_add_point(path, x, y);
}

void pdf_path_curveto(pdf_path *path, double x1, double y1,
                       double x2, double y2, double x3, double y3)
{
    double x0, y0;
    int i;

    if (path == NULL) return;
    if (!path->has_current)
    {
        /* sin punto actual: el primer control pasa a ser el inicio */
        pdf_path_begin_subpath(path, x1, y1);
    }

    x0 = path->cur_x;
    y0 = path->cur_y;

    for (i = 1; i <= PDF_BEZIER_SEGMENTS; i++)
    {
        double t  = (double)i / (double)PDF_BEZIER_SEGMENTS;
        double mt = 1.0 - t;
        double a  = mt * mt * mt;
        double b  = 3.0 * mt * mt * t;
        double c  = 3.0 * mt * t * t;
        double d  = t * t * t;
        double x  = a * x0 + b * x1 + c * x2 + d * x3;
        double y  = a * y0 + b * y1 + c * y2 + d * y3;

        pdf_path_add_point(path, x, y);
    }
}

void pdf_path_close(pdf_path *path)
{
    if (path == NULL || path->n_subpaths == 0) return;

    path->subpath_closed[path->n_subpaths - 1] = 1;
    path->cur_x = path->start_x;
    path->cur_y = path->start_y;
}

void pdf_path_rect_corners(pdf_path *path,
                            double x0, double y0, double x1, double y1,
                            double x2, double y2, double x3, double y3)
{
    if (path == NULL) return;
    pdf_path_moveto(path, x0, y0);
    pdf_path_lineto(path, x1, y1);
    pdf_path_lineto(path, x2, y2);
    pdf_path_lineto(path, x3, y3);
    pdf_path_close(path);
}
