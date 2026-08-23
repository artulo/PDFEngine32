/* pdf_device.c - interfaz generica de devices */
#include "pdf_device.h"

void pdf_device_init(pdf_device *dev, const pdf_device_ops *ops, void *user)
{
    if (dev == NULL)
        return;
    dev->ops = ops;
    dev->user = user;
}

void pdf_device_reset(pdf_device *dev)
{
    if (dev == NULL)
        return;
    dev->ops = NULL;
    dev->user = NULL;
}

void pdf_device_emit(pdf_device *dev, const char *opname,
                     pdf_obj **args, int nargs)
{
    if (dev == NULL || dev->ops == NULL || dev->ops->op == NULL)
        return;
    dev->ops->op(dev->user, opname, args, nargs);
}

void pdf_device_emit_inline_image(pdf_device *dev, pdf_obj *dict_obj,
                                  const unsigned char *data, long len)
{
    if (dev == NULL || dev->ops == NULL || dev->ops->inline_image == NULL)
        return;
    dev->ops->inline_image(dev->user, dict_obj, data, len);
}
