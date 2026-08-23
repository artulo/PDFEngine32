#ifndef PDF_COLORSPACE_H
#define PDF_COLORSPACE_H

#include "pdf_object.h"
#include "pdf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pdf_colorspace_kind_e {
    PDF_CS_DEVICE_GRAY = 0,
    PDF_CS_DEVICE_RGB,
    PDF_CS_DEVICE_CMYK,
    PDF_CS_CAL_GRAY,
    PDF_CS_CAL_RGB,
    PDF_CS_LAB,
    PDF_CS_ICC_BASED,
    PDF_CS_PATTERN,
    PDF_CS_UNKNOWN
} pdf_colorspace_kind;

typedef struct pdf_colorspace_s {
    pdf_colorspace_kind kind;
    int components;
    double gamma;
    double white_x, white_y, white_z;
    double black_x, black_y, black_z;

    /* CalRGB matrix, row-major. */
    double matrix[9];

    /* Lab range. Defaults follow PDF if /Range is absent. */
    double lab_amin, lab_amax;
    double lab_bmin, lab_bmax;
} pdf_colorspace;

void pdf_colorspace_init(pdf_colorspace *cs);
int pdf_colorspace_from_obj(const pdf_obj *obj, pdf_colorspace *cs);
int pdf_colorspace_from_name(const char *name, pdf_colorspace *cs);
void pdf_colorspace_convert(const pdf_colorspace *cs, const double *v, int n, pdf_color *out);

#ifdef __cplusplus
}
#endif
#endif
