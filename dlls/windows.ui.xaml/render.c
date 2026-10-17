/* Windows.UI.Xaml layout and software rendering
 *
 * Copyright (C) 2026 FiveM-Linux contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <math.h>

#include "private.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"

WINE_DEFAULT_DEBUG_CHANNEL(xaml);

#define SUBSAMPLES 5
#define CORNER_SEGMENTS 8
#define INDETERMINATE_PERIOD 2000

struct canvas
{
    HDC hdc;
    DWORD *bits;
    int width, height;
    float *cover;
    double scale;
    DWORD time;
};

struct edge
{
    double x0, y0, x1, y1;
    int dir;
};

static double clamp_size( double value, double min_value, double max_value )
{
    return max( min_value, min( value, max_value ) );
}

/* XAML resolves fonts through DirectWrite, which does not apply GDI font
 * replacements, so skip faces that only exist as a replacement alias. */
static BOOL WINAPI init_font_face( INIT_ONCE *once, void *param, void **context )
{
    static const WCHAR *faces[] = {L"Segoe UI", L"Selawik", L"Arial"};
    LOGFONTW lf = {0};
    WCHAR face[LF_FACESIZE];
    HDC hdc = CreateCompatibleDC( NULL );
    HFONT font, old;
    unsigned int i;

    lf.lfHeight = -12;
    lf.lfCharSet = DEFAULT_CHARSET;
    for (i = 0; i < ARRAY_SIZE(faces); i++)
    {
        if (!RegGetValueW( HKEY_CURRENT_USER, L"Software\\Wine\\Fonts\\Replacements", faces[i],
                           RRF_RT_REG_SZ, NULL, NULL, NULL ))
            continue;
        wcscpy( lf.lfFaceName, faces[i] );
        font = CreateFontIndirectW( &lf );
        old = SelectObject( hdc, font );
        face[0] = 0;
        GetTextFaceW( hdc, ARRAY_SIZE(face), face );
        SelectObject( hdc, old );
        DeleteObject( font );
        if (!wcsicmp( face, faces[i] )) break;
    }
    if (i == ARRAY_SIZE(faces)) i = 0;
    DeleteDC( hdc );
    TRACE( "using %s for the default XAML font.\n", debugstr_w(faces[i]) );
    *context = (void *)faces[i];
    return TRUE;
}

static const WCHAR *default_font_face(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    void *face;

    InitOnceExecuteOnce( &once, init_font_face, NULL, &face );
    return face;
}

static HFONT create_font( const struct element *e, double px )
{
    LOGFONTW lf = {0};

    lf.lfHeight = -(LONG)floor( px + 0.5 );
    lf.lfWeight = e->font_weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
    wcscpy( lf.lfFaceName, default_font_face() );
    return CreateFontIndirectW( &lf );
}

static void text_extent( const struct element *e, HDC hdc, double scale, double *w, double *h )
{
    const WCHAR *text = e->text ? e->text : L"";
    HFONT font = create_font( e, e->font_size * scale ), old;
    TEXTMETRICW tm;
    SIZE size = {0};

    old = SelectObject( hdc, font );
    GetTextMetricsW( hdc, &tm );
    GetTextExtentPoint32W( hdc, text, wcslen( text ), &size );
    SelectObject( hdc, old );
    DeleteObject( font );

    *w = size.cx / scale;
    *h = tm.tmHeight / scale;
}

static void stretch_scale( INT32 stretch, double cw, double ch, double aw, double ah, double *sx, double *sy )
{
    BOOL fw = isfinite( aw ), fh = isfinite( ah );

    *sx = *sy = 1.0;
    if (stretch == STRETCH_NONE || cw <= 0 || ch <= 0 || (!fw && !fh)) return;

    *sx = fw ? aw / cw : ah / ch;
    *sy = fh ? ah / ch : aw / cw;
    if (stretch == STRETCH_UNIFORM) *sx = *sy = min( *sx, *sy );
    else if (stretch == STRETCH_UNIFORM_TO_FILL) *sx = *sy = max( *sx, *sy );
    else if (!fw) *sx = *sy;
    else if (!fh) *sy = *sx;
}

static void measure( struct element *e, double aw, double ah, HDC hdc, double scale )
{
    double mw = e->margin.Left + e->margin.Right, mh = e->margin.Top + e->margin.Bottom;
    double pw = e->padding.Left + e->padding.Right, ph = e->padding.Top + e->padding.Bottom;
    double iw, ih, cw = 0, ch = 0, w, h, sx, sy;
    struct element *child;

    if (e->visibility)
    {
        e->desired_w = e->desired_h = 0;
        return;
    }

    iw = max( 0.0, aw - mw );
    ih = max( 0.0, ah - mh );
    if (!isnan( e->width )) iw = e->width;
    if (!isnan( e->height )) ih = e->height;
    iw = clamp_size( iw, e->min_width, e->max_width );
    ih = clamp_size( ih, e->min_height, e->max_height );

    switch (e->kind)
    {
    case ELEMENT_STACKPANEL:
        LIST_FOR_EACH_ENTRY( child, &e->children, struct element, entry )
        {
            if (e->orientation == ORIENTATION_VERTICAL)
            {
                measure( child, iw, INFINITY, hdc, scale );
                cw = max( cw, child->desired_w );
                ch += child->desired_h;
            }
            else
            {
                measure( child, INFINITY, ih, hdc, scale );
                cw += child->desired_w;
                ch = max( ch, child->desired_h );
            }
        }
        break;

    case ELEMENT_TEXTBLOCK:
        text_extent( e, hdc, scale, &cw, &ch );
        cw += pw;
        ch += ph;
        break;

    case ELEMENT_PROGRESSBAR:
    case ELEMENT_RECTANGLE:
        break;

    case ELEMENT_VIEWBOX:
        LIST_FOR_EACH_ENTRY( child, &e->children, struct element, entry )
        {
            measure( child, INFINITY, INFINITY, hdc, scale );
            stretch_scale( e->stretch, child->desired_w, child->desired_h,
                           isnan( e->width ) && !isfinite( aw ) ? INFINITY : iw,
                           isnan( e->height ) && !isfinite( ah ) ? INFINITY : ih, &sx, &sy );
            cw = child->desired_w * sx;
            ch = child->desired_h * sy;
            break;
        }
        break;

    case ELEMENT_PATH:
    {
        double gw = e->geometry.max_x - e->geometry.min_x, gh = e->geometry.max_y - e->geometry.min_y;

        if (e->stretch == STRETCH_NONE)
        {
            cw = max( 0.0, e->geometry.max_x );
            ch = max( 0.0, e->geometry.max_y );
        }
        else
        {
            stretch_scale( e->stretch, gw, gh, isnan( e->width ) && !isfinite( aw ) ? INFINITY : iw,
                           isnan( e->height ) && !isfinite( ah ) ? INFINITY : ih, &sx, &sy );
            cw = gw * sx;
            ch = gh * sy;
        }
        break;
    }

    default:
        LIST_FOR_EACH_ENTRY( child, &e->children, struct element, entry )
        {
            measure( child, iw - pw, ih - ph, hdc, scale );
            cw = max( cw, child->desired_w );
            ch = max( ch, child->desired_h );
        }
        cw += pw;
        ch += ph;
        break;
    }

    w = isnan( e->width ) ? cw : e->width;
    h = isnan( e->height ) ? ch : e->height;
    w = clamp_size( w, e->min_width, e->max_width ) + mw;
    h = clamp_size( h, e->min_height, e->max_height ) + mh;
    e->desired_w = isfinite( aw ) ? min( w, aw ) : w;
    e->desired_h = isfinite( ah ) ? min( h, ah ) : h;
}

static double align_offset( INT32 align, double slot, double size )
{
    switch (align)
    {
    case HALIGN_LEFT: return 0;
    case HALIGN_RIGHT: return slot - size;
    default: return (slot - size) / 2;
    }
}

static void arrange( struct element *e, double x, double y, double w, double h, struct xform xf )
{
    double mw = e->margin.Left + e->margin.Right, mh = e->margin.Top + e->margin.Bottom;
    double aw, ah, pos;
    struct element *child;

    if (e->visibility) return;

    x += e->margin.Left;
    y += e->margin.Top;
    w = max( 0.0, w - mw );
    h = max( 0.0, h - mh );

    if (!isnan( e->width )) aw = e->width;
    else if (e->halign == HALIGN_STRETCH) aw = w;
    else aw = e->desired_w - mw;
    if (!isnan( e->height )) ah = e->height;
    else if (e->valign == VALIGN_STRETCH) ah = h;
    else ah = e->desired_h - mh;
    aw = clamp_size( aw, e->min_width, e->max_width );
    ah = clamp_size( ah, e->min_height, e->max_height );

    /* VerticalAlignment shares numeric values with HorizontalAlignment */
    e->x = x + align_offset( e->halign, w, aw );
    e->y = y + align_offset( e->valign, h, ah );
    e->actual_w = aw;
    e->actual_h = ah;
    e->xf = xf;

    switch (e->kind)
    {
    case ELEMENT_STACKPANEL:
        pos = 0;
        LIST_FOR_EACH_ENTRY( child, &e->children, struct element, entry )
        {
            if (e->orientation == ORIENTATION_VERTICAL)
            {
                arrange( child, e->x, e->y + pos, aw, child->desired_h, xf );
                pos += child->desired_h;
            }
            else
            {
                arrange( child, e->x + pos, e->y, child->desired_w, ah, xf );
                pos += child->desired_w;
            }
        }
        break;

    case ELEMENT_VIEWBOX:
        LIST_FOR_EACH_ENTRY( child, &e->children, struct element, entry )
        {
            struct xform child_xf;
            double sx, sy;

            stretch_scale( e->stretch, child->desired_w, child->desired_h, aw, ah, &sx, &sy );
            child_xf.sx = xf.sx * sx;
            child_xf.sy = xf.sy * sy;
            child_xf.tx = xf.tx + xf.sx * (e->x + (aw - child->desired_w * sx) / 2);
            child_xf.ty = xf.ty + xf.sy * (e->y + (ah - child->desired_h * sy) / 2);
            arrange( child, 0, 0, child->desired_w, child->desired_h, child_xf );
            break;
        }
        break;

    case ELEMENT_PATH:
    {
        struct geometry *g = &e->geometry;
        double gw = g->max_x - g->min_x, gh = g->max_y - g->min_y, sx, sy;

        if (e->stretch == STRETCH_NONE)
        {
            e->geometry_xf.sx = e->geometry_xf.sy = 1;
            e->geometry_xf.tx = e->x;
            e->geometry_xf.ty = e->y;
        }
        else
        {
            stretch_scale( e->stretch, gw, gh, aw, ah, &sx, &sy );
            e->geometry_xf.sx = sx;
            e->geometry_xf.sy = sy;
            e->geometry_xf.tx = e->x - g->min_x * sx + (aw - gw * sx) / 2;
            e->geometry_xf.ty = e->y - g->min_y * sy + (ah - gh * sy) / 2;
        }
        break;
    }

    default:
        LIST_FOR_EACH_ENTRY( child, &e->children, struct element, entry )
            arrange( child, e->x + e->padding.Left, e->y + e->padding.Top,
                     aw - e->padding.Left - e->padding.Right, ah - e->padding.Top - e->padding.Bottom, xf );
        break;
    }
}

static int compare_crossings( const void *a, const void *b )
{
    const double *x = a, *y = b;
    return x[0] < y[0] ? -1 : x[0] > y[0];
}

static void add_span( float *cover, int width, double xa, double xb, float weight )
{
    int ia, ib, i;

    xa = max( 0.0, xa );
    xb = min( (double)width, xb );
    if (xb <= xa) return;
    ia = (int)xa;
    ib = (int)xb;
    if (ia == ib)
    {
        cover[ia] += (xb - xa) * weight;
        return;
    }
    cover[ia] += (ia + 1 - xa) * weight;
    for (i = ia + 1; i < ib; i++) cover[i] += weight;
    if (ib < width) cover[ib] += (xb - ib) * weight;
}

static void blend_pixel( DWORD *dst, DWORD argb, float alpha )
{
    DWORD d = *dst;
    float inv;
    int r, g, b;

    if (alpha <= 0) return;
    if (alpha > 1) alpha = 1;
    inv = 1 - alpha;
    r = ((argb >> 16) & 0xff) * alpha + ((d >> 16) & 0xff) * inv + 0.5f;
    g = ((argb >> 8) & 0xff) * alpha + ((d >> 8) & 0xff) * inv + 0.5f;
    b = (argb & 0xff) * alpha + (d & 0xff) * inv + 0.5f;
    *dst = 0xff000000 | (r << 16) | (g << 8) | b;
}

/* anti-aliased polygon fill in device pixels */
static void fill_polygons( struct canvas *c, const struct point *const *polys, const UINT *counts, UINT npolys,
                           BOOL nonzero, DWORD argb, double opacity )
{
    struct edge *edges;
    double (*cross)[2], ymin = INFINITY, ymax = -INFINITY;
    float alpha = ((argb >> 24) / 255.0f) * opacity;
    UINT nedges = 0, total = 0, i, j;
    int y, s, x;

    if (alpha <= 0) return;
    for (i = 0; i < npolys; i++) total += counts[i];
    if (!total) return;
    if (!(edges = malloc( total * sizeof(*edges) ))) return;
    if (!(cross = malloc( total * sizeof(*cross) )))
    {
        free( edges );
        return;
    }

    for (i = 0; i < npolys; i++)
    {
        for (j = 0; j < counts[i]; j++)
        {
            const struct point *a = &polys[i][j], *b = &polys[i][(j + 1) % counts[i]];
            if (a->y == b->y) continue;
            edges[nedges].x0 = a->x;
            edges[nedges].y0 = a->y;
            edges[nedges].x1 = b->x;
            edges[nedges].y1 = b->y;
            edges[nedges].dir = a->y < b->y ? 1 : -1;
            ymin = min( ymin, min( a->y, b->y ) );
            ymax = max( ymax, max( a->y, b->y ) );
            nedges++;
        }
    }

    for (y = max( 0, (int)floor( ymin ) ); y < min( c->height, (int)ceil( ymax ) ); y++)
    {
        memset( c->cover, 0, (c->width + 1) * sizeof(float) );
        for (s = 0; s < SUBSAMPLES; s++)
        {
            double sy = y + (s + 0.5) / SUBSAMPLES;
            UINT n = 0;
            int winding = 0;

            for (i = 0; i < nedges; i++)
            {
                const struct edge *e = &edges[i];
                double lo = min( e->y0, e->y1 ), hi = max( e->y0, e->y1 );
                if (sy < lo || sy >= hi) continue;
                cross[n][0] = e->x0 + (sy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0);
                cross[n][1] = e->dir;
                n++;
            }
            qsort( cross, n, sizeof(*cross), compare_crossings );
            for (i = 0; i + 1 < n; i++)
            {
                winding += (int)cross[i][1];
                if (nonzero ? winding != 0 : (i & 1) == 0)
                    add_span( c->cover, c->width, cross[i][0], cross[i + 1][0], 1.0f / SUBSAMPLES );
            }
        }
        for (x = 0; x < c->width; x++)
            if (c->cover[x] > 0) blend_pixel( &c->bits[y * c->width + x], argb, c->cover[x] * alpha );
    }

    free( cross );
    free( edges );
}

static UINT round_rect_points( struct point *pts, double x, double y, double w, double h, double r )
{
    static const double corners[4][3] = {{1, 1, 0}, {-1, 1, 0.5}, {-1, -1, 1}, {1, -1, 1.5}};
    UINT n = 0, i, k;

    r = max( 0.0, min( r, min( w, h ) / 2 ) );
    if (r == 0)
    {
        pts[0].x = x; pts[0].y = y;
        pts[1].x = x + w; pts[1].y = y;
        pts[2].x = x + w; pts[2].y = y + h;
        pts[3].x = x; pts[3].y = y + h;
        return 4;
    }
    for (i = 0; i < 4; i++)
    {
        /* corner centers: bottom-right, bottom-left, top-left, top-right */
        double cx = corners[i][0] > 0 ? x + w - r : x + r;
        double cy = corners[i][1] > 0 ? y + h - r : y + r;
        for (k = 0; k <= CORNER_SEGMENTS; k++)
        {
            double a = (corners[i][2] + 0.5 * k / CORNER_SEGMENTS) * M_PI;
            pts[n].x = cx + r * cos( a );
            pts[n].y = cy + r * sin( a );
            n++;
        }
    }
    return n;
}

static void fill_round_rect( struct canvas *c, double x, double y, double w, double h, double r,
                             DWORD argb, double opacity )
{
    struct point pts[4 * (CORNER_SEGMENTS + 1)];
    const struct point *poly = pts;
    UINT n;

    if (w <= 0 || h <= 0) return;
    n = round_rect_points( pts, x, y, w, h, r );
    fill_polygons( c, &poly, &n, 1, TRUE, argb, opacity );
}

static void device_box( const struct element *e, double *x, double *y, double *w, double *h )
{
    *x = e->xf.tx + e->xf.sx * e->x;
    *y = e->xf.ty + e->xf.sy * e->y;
    *w = e->xf.sx * e->actual_w;
    *h = e->xf.sy * e->actual_h;
}

static void paint_text( struct canvas *c, const struct element *e, double opacity )
{
    double x, y, w, h;
    HFONT font, old;
    UINT format = DT_SINGLELINE | DT_NOPREFIX | DT_TOP;
    RECT rect;
    DWORD argb = e->foreground.argb;

    if (!e->text || !*e->text || !e->foreground.set) return;
    if ((argb >> 24) * opacity < 1) return;
    device_box( e, &x, &y, &w, &h );

    rect.left = (LONG)floor( x + e->padding.Left * e->xf.sx + 0.5 );
    rect.top = (LONG)floor( y + e->padding.Top * e->xf.sy + 0.5 );
    rect.right = (LONG)floor( x + w - e->padding.Right * e->xf.sx + 0.5 );
    rect.bottom = (LONG)floor( y + h - e->padding.Bottom * e->xf.sy + 0.5 );

    switch (e->text_alignment)
    {
    case TEXTALIGN_CENTER: format |= DT_CENTER; break;
    case TEXTALIGN_RIGHT: format |= DT_RIGHT; break;
    default: format |= DT_LEFT; break;
    }

    font = create_font( e, e->font_size * e->xf.sy );
    old = SelectObject( c->hdc, font );
    SetBkMode( c->hdc, TRANSPARENT );
    SetTextColor( c->hdc, RGB( (argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff ) );
    DrawTextW( c->hdc, e->text, -1, &rect, format );
    SelectObject( c->hdc, old );
    DeleteObject( font );
    GdiFlush();
}

static void paint_progress( struct canvas *c, const struct element *e, double opacity )
{
    double x, y, w, h, r, range = e->maximum - e->minimum, sx, ex;

    device_box( e, &x, &y, &w, &h );
    r = e->corner_radius * e->xf.sy;

    if (e->background.set) fill_round_rect( c, x, y, w, h, r, e->background.argb, opacity );
    if (!e->foreground.set) return;

    if (e->indeterminate)
    {
        double phase = (c->time % INDETERMINATE_PERIOD) / (double)INDETERMINATE_PERIOD, seg = w * 0.4;
        sx = max( x, x - seg + phase * (w + seg) );
        ex = min( x + w, x - seg + phase * (w + seg) + seg );
    }
    else
    {
        sx = x;
        ex = x + (range > 0 ? w * max( 0.0, min( 1.0, (e->value - e->minimum) / range ) ) : 0);
    }
    fill_round_rect( c, sx, y, ex - sx, h, r, e->foreground.argb, opacity );
}

static void paint_path( struct canvas *c, const struct element *e, double opacity )
{
    const struct geometry *g = &e->geometry;
    const struct point **polys;
    struct point *pts;
    UINT *counts, total = 0, i, j, k = 0;
    double sx, sy, tx, ty;

    if (!e->fill.set || !g->count) return;
    if (e->stroke.set) FIXME( "path strokes not implemented.\n" );

    for (i = 0; i < g->count; i++) total += g->figures[i].count;
    polys = malloc( g->count * sizeof(*polys) );
    counts = malloc( g->count * sizeof(*counts) );
    pts = malloc( max( total, 1 ) * sizeof(*pts) );
    if (!polys || !counts || !pts) goto done;

    sx = e->xf.sx * e->geometry_xf.sx;
    sy = e->xf.sy * e->geometry_xf.sy;
    tx = e->xf.tx + e->xf.sx * e->geometry_xf.tx;
    ty = e->xf.ty + e->xf.sy * e->geometry_xf.ty;

    for (i = 0; i < g->count; i++)
    {
        polys[i] = &pts[k];
        counts[i] = g->figures[i].count;
        for (j = 0; j < g->figures[i].count; j++, k++)
        {
            pts[k].x = g->figures[i].points[j].x * sx + tx;
            pts[k].y = g->figures[i].points[j].y * sy + ty;
        }
    }
    fill_polygons( c, polys, counts, g->count, g->nonzero, e->fill.argb, opacity );

done:
    free( pts );
    free( counts );
    free( polys );
}

static void paint( struct canvas *c, const struct element *e, double opacity )
{
    const struct element *child;
    double x, y, w, h;

    if (e->visibility) return;
    opacity *= e->opacity;
    if (opacity <= 0) return;

    switch (e->kind)
    {
    case ELEMENT_TEXTBLOCK:
        paint_text( c, e, opacity );
        break;
    case ELEMENT_PROGRESSBAR:
        paint_progress( c, e, opacity );
        break;
    case ELEMENT_PATH:
        paint_path( c, e, opacity );
        break;
    case ELEMENT_RECTANGLE:
        device_box( e, &x, &y, &w, &h );
        if (e->fill.set) fill_round_rect( c, x, y, w, h, 0, e->fill.argb, opacity );
        break;
    case ELEMENT_SWAPCHAINPANEL:
        if (e->swapchain) FIXME( "swapchain content of %p not composited.\n", e );
        break;
    default:
        if (e->background.set)
        {
            device_box( e, &x, &y, &w, &h );
            fill_round_rect( c, x, y, w, h, e->corner_radius * e->xf.sy, e->background.argb, opacity );
        }
        break;
    }

    LIST_FOR_EACH_ENTRY( child, &e->children, struct element, entry )
        paint( c, child, opacity );
}

void render_content( struct element *root, HDC hdc, int width, int height, double dpi_scale, DWORD time )
{
    BITMAPINFO info = {{sizeof(BITMAPINFOHEADER)}};
    struct xform xf = {dpi_scale, dpi_scale, 0, 0};
    struct canvas c = {0};
    HBITMAP bitmap, old;
    DWORD bg = GetSysColor( COLOR_WINDOW );
    int i;

    TRACE( "root %p, hdc %p, %dx%d, dpi scale %s.\n", root, hdc, width, height, wine_dbg_sprintf( "%g", dpi_scale ) );

    if (width <= 0 || height <= 0) return;

    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    if (!(c.hdc = CreateCompatibleDC( hdc ))) return;
    if (!(bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)&c.bits, NULL, 0 )))
    {
        DeleteDC( c.hdc );
        return;
    }
    old = SelectObject( c.hdc, bitmap );
    c.width = width;
    c.height = height;
    c.scale = dpi_scale;
    c.time = time;

    bg = 0xff000000 | (GetRValue( bg ) << 16) | (GetGValue( bg ) << 8) | GetBValue( bg );
    for (i = 0; i < width * height; i++) c.bits[i] = bg;

    if (root && (c.cover = calloc( width + 1, sizeof(float) )))
    {
        EnterCriticalSection( &xaml_cs );
        measure( root, width / dpi_scale, height / dpi_scale, c.hdc, dpi_scale );
        arrange( root, 0, 0, width / dpi_scale, height / dpi_scale, xf );
        TRACE( "root desired %sx%s.\n", wine_dbg_sprintf( "%g", root->desired_w ), wine_dbg_sprintf( "%g", root->desired_h ) );
        paint( &c, root, 1.0 );
        LeaveCriticalSection( &xaml_cs );
        free( c.cover );
    }

    BitBlt( hdc, 0, 0, width, height, c.hdc, 0, 0, SRCCOPY );
    SelectObject( c.hdc, old );
    DeleteObject( bitmap );
    DeleteDC( c.hdc );
}
