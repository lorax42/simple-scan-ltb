/*
 * Copyright (C) 2009 Canonical Ltd.
 * Author: Robert Ancell <robert.ancell@canonical.com>
 * 
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See http://www.gnu.org/copyleft/gpl.html the full text of the
 * license.
 */

#include <math.h>

#include "page-view.h"

enum {
    CHANGED,
    SIZE_CHANGED,
    LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0, };

enum {
    PROP_0,
    PROP_PAGE
};

typedef enum
{
    CROP_NONE = 0,
    CROP_MIDDLE,
    CROP_TOP,
    CROP_BOTTOM,
    CROP_LEFT,
    CROP_RIGHT,
    CROP_TOP_LEFT,
    CROP_TOP_RIGHT,
    CROP_BOTTOM_LEFT,
    CROP_BOTTOM_RIGHT
} CropLocation;

struct PageViewPrivate
{
    /* Page being rendered */
    Page *page;
  
    /* Image to render at current resolution */
    GdkPixbuf *image;
  
    /* Border around image */
    gboolean selected;
    gint border_width;

    /* True if image needs to be regenerated */
    gboolean update_image;

    /* Direction of currently scanned image */
    ScanDirection scan_direction;
  
    /* Next scan line to render */
    gint scan_line;

    /* Dimensions of image to generate */
    gint width, height;

    /* Location to place this page */
    gint x, y;

    CropLocation crop_location;
    gdouble selected_crop_px, selected_crop_py;
    gint selected_crop_x, selected_crop_y;
    gint selected_crop_w, selected_crop_h;

    /* Cursor over this page */
    gint cursor;

    gint animate_n_segments, animate_segment;
    guint animate_timeout;
};

G_DEFINE_TYPE (PageView, page_view, G_TYPE_OBJECT);


PageView *
page_view_new (Page *page)
{
    return g_object_new (PAGE_VIEW_TYPE, "page", page, NULL);
}


Page *
page_view_get_page (PageView *view)
{
    g_return_val_if_fail (view != NULL, NULL);
    return view->priv->page;
}


void
page_view_set_selected (PageView *view, gboolean selected)
{
    g_return_if_fail (view != NULL);
    if ((view->priv->selected && selected) || (!view->priv->selected && !selected))
        return;
    view->priv->selected = selected;
    g_signal_emit (view, signals[CHANGED], 0);  
}


gboolean
page_view_get_selected (PageView *view)
{
    g_return_val_if_fail (view != NULL, FALSE);
    return view->priv->selected;
}


void
page_view_set_x_offset (PageView *view, gint offset)
{
    g_return_if_fail (view != NULL);
    view->priv->x = offset;
}


void
page_view_set_y_offset (PageView *view, gint offset)
{
    g_return_if_fail (view != NULL);
    view->priv->y = offset;
}


gint
page_view_get_x_offset (PageView *view)
{
    g_return_val_if_fail (view != NULL, 0);  
    return view->priv->x;  
}


gint
page_view_get_y_offset (PageView *view)
{
    g_return_val_if_fail (view != NULL, 0);
    return view->priv->y;
}


static guchar
get_sample (const guchar *line, gint x, gint depth, gint sample)
{
    // FIXME
    return 0xFF;
}


static void
get_pixel (Page *page, gint x, gint y, guchar *pixel)
{
    gint t, depth, n_channels;
    const guchar *p, *line;

    switch (page_get_scan_direction (page))
    {
    case TOP_TO_BOTTOM:
        break;
    case BOTTOM_TO_TOP:
        x = page_get_scan_width (page) - x - 1;
        y = page_get_scan_height (page) - y - 1;
        break;
    case LEFT_TO_RIGHT:
        t = x;
        x = page_get_scan_width (page) - y - 1;
        y = t;
        break;
    case RIGHT_TO_LEFT:
        t = x;
        x = y;
        y = page_get_scan_height (page) - t - 1;
        break;
    }

    depth = page_get_depth (page);
    n_channels = page_get_n_channels (page);
    line = page_get_pixels (page) + page_get_rowstride (page) * y;

    /* Optimise for 8 bit images */
    if (depth == 8 && n_channels == 3) {
        p = line + x * n_channels;
        pixel[0] = p[0];
        pixel[1] = p[1];
        pixel[2] = p[2];
        return;
    }
    else if (depth == 8 && n_channels == 1) {
        p = line + x;
        pixel[0] = pixel[1] = pixel[2] = p[0];
        return;
    }

    /* Optimise for bitmaps */
    else if (depth == 1 && n_channels == 1) {
        p = line + (x / 8);
        pixel[0] = pixel[1] = pixel[2] = p[0] & (0x80 >> (x % 8)) ? 0x00 : 0xFF;
        return;
    }

    /* Optimise for 2 bit images */
    else if (depth == 2 && n_channels == 1) {
        gint sample;
        gint block_shift[4] = { 6, 4, 2, 0 };

        p = line + (x / 4);
        sample = (p[0] >> block_shift[x % 4]) & 0x3;
        sample = sample * 255 / 3;

        pixel[0] = pixel[1] = pixel[2] = sample;
        return;
    }

    /* Use slow method */
    pixel[0] = get_sample (line, x, depth, x * n_channels);
    pixel[0] = get_sample (line, x, depth, x * n_channels + 1);
    pixel[0] = get_sample (line, x, depth, x * n_channels + 2);
}


static void
set_pixel (Page *page,
           double l, double r, double t, double b, guchar *pixel)
{
    gint x, y;
    gint L, R, T, B;
    guchar p[3];
    double scale, red, green, blue;
  
    /* Decimation:
     * 
     * Target pixel is defined by (t,l)-(b,r)
     * It touches 16 pixels in original image
     * It completely covers 4 pixels in original image (T,L)-(B,R)
     * Add covered pixels and add weighted partially covered pixels.
     * Divide by total area.
     * 
     *      l  L           R   r 
     *   +-----+-----+-----+-----+
     *   |     |     |     |     |
     * t |  +--+-----+-----+---+ |
     * T +--+--+-----+-----+---+-+
     *   |  |  |     |     |   | |
     *   |  |  |     |     |   | |
     *   +--+--+-----+-----+---+-+
     *   |  |  |     |     |   | |
     *   |  |  |     |     |   | |
     * B +--+--+-----+-----+---+-+
     *   |  |  |     |     |   | |
     * b |  +--+-----+-----+---+ |
     *   +-----+-----+-----+-----+
     * 
     * 
     * Interpolation:
     * 
     *             l    r
     *   +-----+-----+-----+-----+
     *   |     |     |     |     |
     *   |     |     |     |     |
     *   +-----+-----+-----+-----+
     * t |     |   +-+--+  |     |
     *   |     |   | |  |  |     |
     *   +-----+---+-+--+--+-----+
     * b |     |   +-+--+  |     |
     *   |     |     |     |     |
     *   +-----+-----+-----+-----+
     *   |     |     |     |     |
     *   |     |     |     |     |
     *   +-----+-----+-----+-----+
     * 
     * Same again, just no completely covered pixels.
     */
  
    L = l;
    if (L != l)
        L++;
    R = r;
    T = t;
    if (T != t)
        T++;
    B = b;
  
    red = green = blue = 0.0;

    /* Target can fit inside one source pixel
     * +-----+
     * |     |
     * | +--+|      +-----+-----+      +-----+      +-----+      +-----+
     * +-+--++  or  |   +-++    |  or  | +-+ |  or  | +--+|  or  |  +--+
     * | +--+|      |   +-++    |      | +-+ |      | |  ||      |  |  |
     * |     |      +-----+-----+      +-----+      +-+--++      +--+--+
     * +-----+
     */
    if ((r - l <= 1.0 && (gint)r == (gint)l) || (b - t <= 1.0 && (gint)b == (gint)t)) {
        /* Inside */
        if ((gint)l == (gint)r || (gint)t == (gint)b) {
            get_pixel (page, (gint)l, (gint)t, p);
            pixel[0] = p[0];
            pixel[1] = p[1];
            pixel[2] = p[2];
            return;
        }

        /* Stradling horizontal edge */
        if (L > R) {
            get_pixel (page, R, T-1, p);
            red   += p[0] * (r-l)*(T-t);
            green += p[1] * (r-l)*(T-t);
            blue  += p[2] * (r-l)*(T-t);
            for (y = T; y < B; y++) {
                get_pixel (page, R, y, p);
                red   += p[0] * (r-l);
                green += p[1] * (r-l);
                blue  += p[2] * (r-l);
            }
            get_pixel (page, R, B, p);
            red   += p[0] * (r-l)*(b-B);
            green += p[1] * (r-l)*(b-B);
            blue  += p[2] * (r-l)*(b-B);
        }
        /* Stradling vertical edge */
        else {
            get_pixel (page, L - 1, B, p);
            red   += p[0] * (b-t)*(L-l);
            green += p[1] * (b-t)*(L-l);
            blue  += p[2] * (b-t)*(L-l);
            for (x = L; x < R; x++) {
                get_pixel (page, x, B, p);
                red   += p[0] * (b-t);
                green += p[1] * (b-t);
                blue  += p[2] * (b-t);
            }
            get_pixel (page, R, B, p);
            red   += p[0] * (b-t)*(r-R);
            green += p[1] * (b-t)*(r-R);
            blue  += p[2] * (b-t)*(r-R);
        }

        scale = 1.0 / ((r - l) * (b - t));
        pixel[0] = (guchar)(red * scale + 0.5);
        pixel[1] = (guchar)(green * scale + 0.5);
        pixel[2] = (guchar)(blue * scale + 0.5);
        return;
    }

    /* Add the middle pixels */
    for (x = L; x < R; x++) {
        for (y = T; y < B; y++) {
            get_pixel (page, x, y, p);
            red   += p[0];
            green += p[1];
            blue  += p[2];
        }
    }

    /* Add the weighted top and bottom pixels */
    for (x = L; x < R; x++) {
        if (t != T) {
            get_pixel (page, x, T - 1, p);
            red   += p[0] * (T - t);
            green += p[1] * (T - t);
            blue  += p[2] * (T - t);
        }

        if (b != B) {
            get_pixel (page, x, B, p);
            red   += p[0] * (b - B);
            green += p[1] * (b - B);
            blue  += p[2] * (b - B);
        }
    }

    /* Add the left and right pixels */
    for (y = T; y < B; y++) {
        if (l != L) {
            get_pixel (page, L - 1, y, p);
            red   += p[0] * (L - l);
            green += p[1] * (L - l);
            blue  += p[2] * (L - l);
        }

        if (r != R) {
            get_pixel (page, R, y, p);
            red   += p[0] * (r - R);
            green += p[1] * (r - R);
            blue  += p[2] * (r - R);
        }
    }
  
    /* Add the corner pixels */
    if (l != L && t != T) {
        get_pixel (page, L - 1, T - 1, p);
        red   += p[0] * (L - l)*(T - t);
        green += p[1] * (L - l)*(T - t);
        blue  += p[2] * (L - l)*(T - t);
    }
    if (r != R && t != T) {
        get_pixel (page, R, T - 1, p);
        red   += p[0] * (r - R)*(T - t);
        green += p[1] * (r - R)*(T - t);
        blue  += p[2] * (r - R)*(T - t);
    }
    if (r != R && b != B) {
        get_pixel (page, R, B, p);
        red   += p[0] * (r - R)*(b - B);
        green += p[1] * (r - R)*(b - B);
        blue  += p[2] * (r - R)*(b - B);
    }
    if (l != L && b != B) {
        get_pixel (page, L - 1, B, p);
        red   += p[0] * (L - l)*(b - B);
        green += p[1] * (L - l)*(b - B);
        blue  += p[2] * (L - l)*(b - B);
    }

    /* Scale pixel values and clamp in range [0, 255] */
    scale = 1.0 / ((r - l) * (b - t));
    pixel[0] = (guchar)(red * scale + 0.5);
    pixel[1] = (guchar)(green * scale + 0.5);
    pixel[2] = (guchar)(blue * scale + 0.5);
}


static void
update_preview (Page *page,
                GdkPixbuf **output_image, gint output_width, gint output_height,
                ScanDirection scan_direction, gint old_scan_line, gint scan_line)
{
    guchar *output;
    gint input_width, input_height;
    gint output_rowstride, output_n_channels;
    gint x, y;
    gint L, R, T, B;

    input_width = page_get_width (page);
    input_height = page_get_height (page);
  
    /* Create new image if one does not exist or has changed size */
    if (!*output_image ||
        gdk_pixbuf_get_width (*output_image) != output_width ||
        gdk_pixbuf_get_height (*output_image) != output_height) {
        if (*output_image)
            g_object_unref (*output_image); 
        *output_image = gdk_pixbuf_new (GDK_COLORSPACE_RGB,
                                        FALSE,
                                        8,
                                        output_width,
                                        output_height);

        /* Update entire image */
        L = 0;
        R = output_width - 1;
        T = 0;
        B = output_height - 1;
    }
    /* Otherwise only update changed area */
    else {
        switch (scan_direction) {
        case TOP_TO_BOTTOM:
            L = 0;
            R = output_width - 1;
            T = (gint)((double)old_scan_line * output_height / input_height);
            B = (gint)((double)scan_line * output_height / input_height + 0.5);
            break;
        case LEFT_TO_RIGHT:
            L = (gint)((double)old_scan_line * output_width / input_width);
            R = (gint)((double)scan_line * output_width / input_width + 0.5);
            T = 0;
            B = output_height - 1;
            break;
        case BOTTOM_TO_TOP:
            L = 0;
            R = output_width - 1;
            T = (gint)((double)(input_height - scan_line) * output_height / input_height);
            B = (gint)((double)(input_height - old_scan_line) * output_height / input_height + 0.5);
            break;
        case RIGHT_TO_LEFT:
            L = (gint)((double)(input_width - scan_line) * output_width / input_width);
            R = (gint)((double)(input_width - old_scan_line) * output_width / input_width + 0.5);
            T = 0;
            B = output_height - 1;
            break;
        default:
            L = R = B = T = 0;
            break;
        }
    }

    /* FIXME: There's an off by one error in there somewhere... */
    if (R >= output_width)
        R = output_width - 1;
    if (B >= output_height)
        B = output_height - 1;

    g_return_if_fail (L >= 0);
    g_return_if_fail (R < output_width);
    g_return_if_fail (T >= 0);
    g_return_if_fail (B < output_height);
    g_return_if_fail (*output_image != NULL);
  
    output = gdk_pixbuf_get_pixels (*output_image);
    output_rowstride = gdk_pixbuf_get_rowstride (*output_image);
    output_n_channels = gdk_pixbuf_get_n_channels (*output_image);
  
    if (!page_has_data (page)) {
        for (x = L; x <= R; x++)
            for (y = T; y <= B; y++) {
                guchar *pixel;
                pixel = output + output_rowstride * y + x * output_n_channels;
                pixel[0] = pixel[1] = pixel[2] = 0xFF;
            }
        return; 
    }

    /* Update changed area */
    for (x = L; x <= R; x++) {
        double l, r;

        l = (double)x * input_width / output_width;
        r = (double)(x + 1) * input_width / output_width;

        for (y = T; y <= B; y++) {
            double t, b;

            t = (double)y * input_height / output_height;
            b = (double)(y + 1) * input_height / output_height;

            set_pixel (page,
                       l, r, t, b,
                       output + output_rowstride * y + x * output_n_channels);
        }
    }
}


static gint
get_preview_width (PageView *view)
{
    return view->priv->width - view->priv->border_width * 2;
}


static gint
get_preview_height (PageView *view)
{
    return view->priv->height - view->priv->border_width * 2;
}


static void
update_page_view (PageView *view)
{
    gint old_scan_line, scan_line, left_steps;

    if (!view->priv->update_image)
        return;

    old_scan_line = view->priv->scan_line;
    scan_line = page_get_scan_line (view->priv->page);

    /* Delete old image if scan direction changed */
    left_steps = view->priv->scan_direction - page_get_scan_direction (view->priv->page);
    if (left_steps && view->priv->image) {
        g_object_unref (view->priv->image);
        view->priv->image = NULL;
    }
    view->priv->scan_direction = page_get_scan_direction (view->priv->page);

    update_preview (view->priv->page,
                    &view->priv->image,
                    get_preview_width (view),
                    get_preview_height (view),
                    page_get_scan_direction (view->priv->page), old_scan_line, scan_line);

    view->priv->update_image = FALSE;
    view->priv->scan_line = scan_line;
}


static gint
page_to_screen_x (PageView *view, gint x)
{
    return (double) x * get_preview_width (view) / page_get_width (view->priv->page) + 0.5;
}


static gint
page_to_screen_y (PageView *view, gint y)
{
    return (double) y * get_preview_height (view) / page_get_height (view->priv->page) + 0.5;    
}


static gint
screen_to_page_x (PageView *view, gint x)
{
    return (double) x * page_get_width (view->priv->page) / get_preview_width (view) + 0.5;
}


static gint
screen_to_page_y (PageView *view, gint y)
{
    return (double) y * page_get_height (view->priv->page) / get_preview_height (view) + 0.5;
}


static CropLocation
get_crop_location (PageView *view, gint x, gint y)
{
    gint cx, cy, cw, ch;
    gint dx, dy, dw, dh;
    gint ix, iy;
    gint crop_border = 20;
    gchar *name;

    if (!page_has_crop (view->priv->page))
        return 0;

    page_get_crop (view->priv->page, &cx, &cy, &cw, &ch);
    dx = page_to_screen_x (view, cx);
    dy = page_to_screen_y (view, cy);
    dw = page_to_screen_x (view, cw);
    dh = page_to_screen_y (view, ch);
    ix = x - dx;
    iy = y - dy;

    if (ix < 0 || ix > dw || iy < 0 || iy > dh)
        return CROP_NONE;

    /* Can't resize named crops */
    name = page_get_named_crop (view->priv->page);
    if (name != NULL) {
        g_free (name);
        return CROP_MIDDLE;
    }

    /* Adjust borders so can select */
    if (dw < crop_border * 3)
        crop_border = dw / 3;
    if (dh < crop_border * 3)
        crop_border = dh / 3;

    /* Top left */
    if (ix < crop_border && iy < crop_border)
        return CROP_TOP_LEFT;
    /* Top right */
    if (ix > dw - crop_border && iy < crop_border)
        return CROP_TOP_RIGHT;
    /* Bottom left */
    if (ix < crop_border && iy > dh - crop_border)
        return CROP_BOTTOM_LEFT;
    /* Bottom right */
    if (ix > dw - crop_border && iy > dh - crop_border)
        return CROP_BOTTOM_RIGHT;

    /* Left */
    if (ix < crop_border)
        return CROP_LEFT;
    /* Right */
    if (ix > dw - crop_border)
        return CROP_RIGHT;
    /* Top */
    if (iy < crop_border)
        return CROP_TOP;
    /* Bottom */
    if (iy > dh - crop_border)
        return CROP_BOTTOM;

    /* In the middle */
    return CROP_MIDDLE;
}


void
page_view_button_press (PageView *view, gint x, gint y)
{
    CropLocation location;

    g_return_if_fail (view != NULL);

    /* See if selecting crop */
    location = get_crop_location (view, x, y);;
    if (location != CROP_NONE) {
        view->priv->crop_location = location;
        view->priv->selected_crop_px = x;
        view->priv->selected_crop_py = y;
        page_get_crop (view->priv->page,
                       &view->priv->selected_crop_x,
                       &view->priv->selected_crop_y,
                       &view->priv->selected_crop_w,
                       &view->priv->selected_crop_h);
    }
}


void
page_view_motion (PageView *view, gint x, gint y)
{
    gint pw, ph;
    gint cx, cy, cw, ch, dx, dy;
    gint new_x, new_y, new_w, new_h;
    CropLocation location;
    gint cursor;
    gint min_size;
  
    min_size = screen_to_page_x (view, 15);

    g_return_if_fail (view != NULL);
  
    location = get_crop_location (view, x, y);
    switch (location) {
    case CROP_MIDDLE:
        cursor = GDK_HAND1;
        break;
    case CROP_TOP:
        cursor = GDK_TOP_SIDE;
        break;
    case CROP_BOTTOM:
        cursor = GDK_BOTTOM_SIDE;
        break;
    case CROP_LEFT:
        cursor = GDK_LEFT_SIDE;
        break;
    case CROP_RIGHT:
        cursor = GDK_RIGHT_SIDE;
        break;
    case CROP_TOP_LEFT:
        cursor = GDK_TOP_LEFT_CORNER;
        break;
    case CROP_TOP_RIGHT:
        cursor = GDK_TOP_RIGHT_CORNER;
        break;
    case CROP_BOTTOM_LEFT:
        cursor = GDK_BOTTOM_LEFT_CORNER;
        break;
    case CROP_BOTTOM_RIGHT:
        cursor = GDK_BOTTOM_RIGHT_CORNER;
        break;
    default:
        cursor = GDK_ARROW;
        break;
    }

    if (view->priv->crop_location == CROP_NONE) {
        view->priv->cursor = cursor;
        return;
    }

    /* Move the crop */  
    pw = page_get_width (view->priv->page);
    ph = page_get_height (view->priv->page);
    page_get_crop (view->priv->page, &cx, &cy, &cw, &ch);

    dx = screen_to_page_x (view, x - view->priv->selected_crop_px);
    dy = screen_to_page_y (view, y - view->priv->selected_crop_py);

    new_x = view->priv->selected_crop_x;
    new_y = view->priv->selected_crop_y;
    new_w = view->priv->selected_crop_w;
    new_h = view->priv->selected_crop_h;

    /* Limit motion to remain within page and minimum crop size */
    if (view->priv->crop_location == CROP_TOP_LEFT ||
        view->priv->crop_location == CROP_LEFT ||
        view->priv->crop_location == CROP_BOTTOM_LEFT) {
        if (dx > new_w - min_size)
            dx = new_w - min_size;
        if (new_x + dx < 0)
            dx = -new_x;
    }
    if (view->priv->crop_location == CROP_TOP_LEFT ||
        view->priv->crop_location == CROP_TOP ||
        view->priv->crop_location == CROP_TOP_RIGHT) {
        if (dy > new_h - min_size)
            dy = new_h - min_size;
        if (new_y + dy < 0)
            dy = -new_y;
    }
     
    if (view->priv->crop_location == CROP_TOP_RIGHT ||
        view->priv->crop_location == CROP_RIGHT ||
        view->priv->crop_location == CROP_BOTTOM_RIGHT) {
        if (dx < min_size - new_w)
            dx = min_size - new_w;
        if (new_x + new_w + dx > pw)
            dx = pw - new_x - new_w;
    }
    if (view->priv->crop_location == CROP_BOTTOM_LEFT ||
        view->priv->crop_location == CROP_BOTTOM ||
        view->priv->crop_location == CROP_BOTTOM_RIGHT) {
        if (dy < min_size - new_h)
            dy = min_size - new_h;
        if (new_y + new_h + dy > ph)
            dy = ph - new_y - new_h;
    }
    if (view->priv->crop_location == CROP_MIDDLE) {
        if (new_x + dx + new_w > pw)
            dx = pw - new_x - new_w;
        if (new_x + dx < 0)
            dx = -new_x;
        if (new_y + dy + new_h > ph)
            dy = ph - new_y - new_h;
        if (new_y + dy  < 0)
            dy = -new_y;
    }

    /* Move crop */
    if (view->priv->crop_location == CROP_MIDDLE) {
        new_x += dx;
        new_y += dy;          
    }
    if (view->priv->crop_location == CROP_TOP_LEFT ||
        view->priv->crop_location == CROP_LEFT ||
        view->priv->crop_location == CROP_BOTTOM_LEFT) 
    {
        new_x += dx;
        new_w -= dx;
    }
    if (view->priv->crop_location == CROP_TOP_LEFT ||
        view->priv->crop_location == CROP_TOP ||
        view->priv->crop_location == CROP_TOP_RIGHT) {
        new_y += dy;
        new_h -= dy;
    }
 
    if (view->priv->crop_location == CROP_TOP_RIGHT ||
        view->priv->crop_location == CROP_RIGHT ||
        view->priv->crop_location == CROP_BOTTOM_RIGHT) {
        new_w += dx;
    }
    if (view->priv->crop_location == CROP_BOTTOM_LEFT ||
        view->priv->crop_location == CROP_BOTTOM ||
        view->priv->crop_location == CROP_BOTTOM_RIGHT) {
        new_h += dy;
    }

    page_move_crop (view->priv->page, new_x, new_y);

    /* If reshaped crop, must be a custom crop */
    if (new_w != cw || new_h != ch)
        page_set_custom_crop (view->priv->page, new_w, new_h);
}


void
page_view_button_release (PageView *view, gint x, gint y)
{
    g_return_if_fail (view != NULL);

    /* Complete crop */
    view->priv->crop_location = CROP_NONE;
    g_signal_emit (view, signals[CHANGED], 0);
}


gint
page_view_get_cursor (PageView *view)
{
    g_return_val_if_fail (view != NULL, 0);
    return view->priv->cursor;
}


static gboolean
animation_cb (PageView *view)
{
    view->priv->animate_segment = (view->priv->animate_segment + 1) % view->priv->animate_n_segments;
    g_signal_emit (view, signals[CHANGED], 0);
    return TRUE;
}


static void
update_animation (PageView *view)
{
    gboolean animate, is_animating;

    animate = page_is_scanning (view->priv->page) && !page_has_data (view->priv->page);
    is_animating = view->priv->animate_timeout != 0;
    if (animate == is_animating)
        return;
  
    if (animate) {
        view->priv->animate_segment = 0;
        if (view->priv->animate_timeout == 0)
            view->priv->animate_timeout = g_timeout_add (150, (GSourceFunc) animation_cb, view);
    }
    else
    {
        if (view->priv->animate_timeout != 0)
            g_source_remove (view->priv->animate_timeout);
        view->priv->animate_timeout = 0;
    }
}


void
page_view_render (PageView *view, cairo_t *context)
{
    gint width, height;

    g_return_if_fail (view != NULL);
    g_return_if_fail (context != NULL);

    update_animation (view);
    update_page_view (view);

    width = get_preview_width (view);
    height = get_preview_height (view);

    cairo_set_line_width (context, 1);
    cairo_translate (context, view->priv->x, view->priv->y);

    /* Draw page border */
    cairo_set_source_rgb (context, 0, 0, 0);
    cairo_set_line_width (context, view->priv->border_width);
    cairo_rectangle (context,
                     (double)view->priv->border_width / 2,
                     (double)view->priv->border_width / 2,
                     view->priv->width - view->priv->border_width,
                     view->priv->height - view->priv->border_width);
    cairo_stroke (context);

    /* Draw image */
    cairo_translate (context, view->priv->border_width, view->priv->border_width);
    gdk_cairo_set_source_pixbuf (context, view->priv->image, 0, 0);
    cairo_paint (context);

    /* Draw throbber */
    if (page_is_scanning (view->priv->page) && !page_has_data (view->priv->page)) {
        gdouble inner_radius, outer_radius, x, y, arc, offset = 0.0;
        gint i;

        if (width > height)
            outer_radius = 0.15 * width;
        else
            outer_radius = 0.15 * height;
        arc = M_PI / view->priv->animate_n_segments;

        /* Space circles */
        x = outer_radius * sin (arc);
        y = outer_radius * (cos (arc) - 1.0);
        inner_radius = 0.6 * sqrt (x*x + y*y);

        for (i = 0; i < view->priv->animate_n_segments; i++, offset += arc * 2) {
            x = width / 2 + outer_radius * sin (offset);
            y = height / 2 - outer_radius * cos (offset);
            cairo_arc (context, x, y, inner_radius, 0, 2 * M_PI);

            if (i == view->priv->animate_segment) {
                cairo_set_source_rgb (context, 0.75, 0.75, 0.75);
                cairo_fill_preserve (context);
            }

            cairo_set_source_rgb (context, 0.5, 0.5, 0.5);
            cairo_stroke (context);
        }
    }

    /* Draw scan line */
    if (page_is_scanning (view->priv->page) && page_get_scan_line (view->priv->page) > 0) {
        gint scan_line;
        double s;
        double x1, y1, x2, y2;

        scan_line = page_get_scan_line (view->priv->page);

        switch (page_get_scan_direction (view->priv->page)) {
        case TOP_TO_BOTTOM:
            s = page_to_screen_y (view, scan_line);
            x1 = 0; y1 = s + 0.5;
            x2 = width; y2 = s + 0.5;
            break;
        case BOTTOM_TO_TOP:
            s = page_to_screen_y (view, scan_line);
            x1 = 0; y1 = height - s + 0.5;
            x2 = width; y2 = height - s + 0.5;
            break;
        case LEFT_TO_RIGHT:
            s = page_to_screen_x (view, scan_line);
            x1 = s + 0.5; y1 = 0;
            x2 = s + 0.5; y2 = height;
            break;
        case RIGHT_TO_LEFT:
            s = page_to_screen_x (view, scan_line);
            x1 = width - s + 0.5; y1 = 0;
            x2 = width - s + 0.5; y2 = height;
            break;
        default:
            x1 = y1 = x2 = y2 = 0;
            break;
        }

        cairo_move_to (context, x1, y1);
        cairo_line_to (context, x2, y2);
        cairo_set_source_rgb (context, 1.0, 0.0, 0.0);
        cairo_stroke (context);
    }
    
    /* Draw crop */
    if (page_has_crop (view->priv->page)) {
        gint x, y, crop_width, crop_height;
        gdouble dx, dy, dw, dh;

        page_get_crop (view->priv->page, &x, &y, &crop_width, &crop_height);

        dx = page_to_screen_x (view, x);
        dy = page_to_screen_y (view, y);
        dw = page_to_screen_x (view, crop_width);
        dh = page_to_screen_y (view, crop_height);
        
        /* Shade out cropped area */
        cairo_rectangle (context,
                         0, 0,
                         width, height);
        cairo_new_sub_path (context);
        cairo_rectangle (context, dx, dy, dw, dh);
        cairo_set_fill_rule (context, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_set_source_rgba (context, 0.25, 0.25, 0.25, 0.2);
        cairo_fill (context);
        
        /* Show new edge */
        cairo_rectangle (context, dx - 1.5, dy - 1.5, dw + 3, dh + 3);
        cairo_set_source_rgb (context, 1.0, 1.0, 1.0);
        cairo_stroke (context);
        cairo_rectangle (context, dx - 0.5, dy - 0.5, dw + 1, dh + 1);
        cairo_set_source_rgb (context, 0.0, 0.0, 0.0);
        cairo_stroke (context);
    }
}


void
page_view_set_width (PageView *view, gint width)
{
    gint height;

    g_return_if_fail (view != NULL);

    // FIXME: Automatically update when get updated image
    height = (double)width * page_get_height (view->priv->page) / page_get_width (view->priv->page);
    if (view->priv->width == width && view->priv->height == height)
        return;

    view->priv->width = width;
    view->priv->height = height;
  
    /* Regenerate image */
    view->priv->update_image = TRUE;

    g_signal_emit (view, signals[SIZE_CHANGED], 0);
    g_signal_emit (view, signals[CHANGED], 0);
}


void
page_view_set_height (PageView *view, gint height)
{
    gint width;

    g_return_if_fail (view != NULL);

    // FIXME: Automatically update when get updated image
    width = (double)height * page_get_width (view->priv->page) / page_get_height (view->priv->page);
    if (view->priv->width == width && view->priv->height == height)
        return;

    view->priv->width = width;
    view->priv->height = height;
  
    /* Regenerate image */
    view->priv->update_image = TRUE;

    g_signal_emit (view, signals[SIZE_CHANGED], 0);  
    g_signal_emit (view, signals[CHANGED], 0);
}


gint
page_view_get_width (PageView *view)
{
    g_return_val_if_fail (view != NULL, 0);
    return view->priv->width;
}


gint
page_view_get_height (PageView *view)
{
    g_return_val_if_fail (view != NULL, 0);
    return view->priv->height;
}


static void
page_pixels_changed_cb (Page *p, PageView *view)
{
    /* Regenerate image */
    view->priv->update_image = TRUE;
    g_signal_emit (view, signals[CHANGED], 0);
}


static void
page_size_changed_cb (Page *p, PageView *view)
{
    /* Regenerate image */
    view->priv->update_image = TRUE;
    g_signal_emit (view, signals[SIZE_CHANGED], 0);
    g_signal_emit (view, signals[CHANGED], 0);
}


static void
page_overlay_changed_cb (Page *p, PageView *view)
{
    g_signal_emit (view, signals[CHANGED], 0);
}


static void
scan_direction_changed_cb (Page *p, PageView *view)
{
    /* Regenerate image */
    view->priv->update_image = TRUE;
    g_signal_emit (view, signals[SIZE_CHANGED], 0);
    g_signal_emit (view, signals[CHANGED], 0);
}


static void
page_view_set_page (PageView *view, Page *page)
{
    g_return_if_fail (view != NULL);
    g_return_if_fail (view->priv->page == NULL);

    view->priv->page = g_object_ref (page);
    g_signal_connect (view->priv->page, "pixels-changed", G_CALLBACK (page_pixels_changed_cb), view);
    g_signal_connect (view->priv->page, "size-changed", G_CALLBACK (page_size_changed_cb), view);
    g_signal_connect (view->priv->page, "crop-changed", G_CALLBACK (page_overlay_changed_cb), view);
    g_signal_connect (view->priv->page, "scan-line-changed", G_CALLBACK (page_overlay_changed_cb), view);
    g_signal_connect (view->priv->page, "scan-direction-changed", G_CALLBACK (scan_direction_changed_cb), view);
}


static void
page_view_set_property (GObject      *object,
                        guint         prop_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
    PageView *self;

    self = PAGE_VIEW (object);

    switch (prop_id) {
    case PROP_PAGE:
        page_view_set_page (self, g_value_get_object (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}


static void
page_view_get_property (GObject    *object,
                        guint       prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
    PageView *self;

    self = PAGE_VIEW (object);

    switch (prop_id) {
    case PROP_PAGE:
        g_value_set_object (value, self->priv->page);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}


static void
page_view_finalize (GObject *object)
{
    PageView *view = PAGE_VIEW (object);
    g_object_unref (view->priv->page);
    view->priv->page = NULL;
    if (view->priv->image)
        g_object_unref (view->priv->image);
    view->priv->image = NULL;
    if (view->priv->animate_timeout != 0)
        g_source_remove (view->priv->animate_timeout);
    view->priv->animate_timeout = 0;
    G_OBJECT_CLASS (page_view_parent_class)->finalize (object);
}


static void
page_view_class_init (PageViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->get_property = page_view_get_property;
    object_class->set_property = page_view_set_property;
    object_class->finalize = page_view_finalize;

    signals[CHANGED] =
        g_signal_new ("changed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (PageViewClass, changed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);
    signals[SIZE_CHANGED] =
        g_signal_new ("size-changed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (PageViewClass, size_changed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

    g_type_class_add_private (klass, sizeof (PageViewPrivate));

    g_object_class_install_property (object_class,
                                     PROP_PAGE,
                                     g_param_spec_object ("page",
                                                          "page",
                                                          "Page being rendered",
                                                          PAGE_TYPE,
                                                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}


static void
page_view_init (PageView *view)
{
    view->priv = G_TYPE_INSTANCE_GET_PRIVATE (view, PAGE_VIEW_TYPE, PageViewPrivate);
    view->priv->update_image = TRUE;
    view->priv->cursor = GDK_ARROW;
    view->priv->border_width = 1;
    view->priv->animate_n_segments = 7;
}
