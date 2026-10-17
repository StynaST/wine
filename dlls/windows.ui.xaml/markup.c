/* WinRT Windows.UI.Xaml markup value parsing
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

WINE_DEFAULT_DEBUG_CHANNEL(xaml);

#define BEZIER_SEGMENTS 16

static const struct
{
    const WCHAR *name;
    DWORD argb;
}
named_colors[] =
{
    {L"Black",       0xff000000},
    {L"Blue",        0xff0000ff},
    {L"Gray",        0xff808080},
    {L"Green",       0xff008000},
    {L"Orange",      0xffffa500},
    {L"Red",         0xffff0000},
    {L"Transparent", 0x00ffffff},
    {L"White",       0xffffffff},
    {L"Yellow",      0xffffff00},
};

static const WCHAR *skip_space( const WCHAR *p )
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

BOOL parse_color( const WCHAR *str, DWORD *argb )
{
    unsigned int i, len;
    DWORD value = 0;
    const WCHAR *p;

    str = skip_space( str );
    if (*str == '#')
    {
        for (p = str + 1, len = 0; iswxdigit( *p ); p++, len++)
            value = (value << 4) | (*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
        if (*skip_space( p )) return FALSE;
        switch (len)
        {
        case 3: /* #RGB */
            value = ((value & 0xf00) << 12) | ((value & 0xf0) << 8) | ((value & 0xf) << 4);
            *argb = 0xff000000 | value | (value >> 4);
            return TRUE;
        case 4: /* #ARGB */
            value = ((value & 0xf000) << 16) | ((value & 0xf00) << 12) | ((value & 0xf0) << 8) | ((value & 0xf) << 4);
            *argb = value | (value >> 4);
            return TRUE;
        case 6: *argb = 0xff000000 | value; return TRUE;
        case 8: *argb = value; return TRUE;
        }
        return FALSE;
    }

    for (i = 0; i < ARRAY_SIZE(named_colors); i++)
    {
        if (!wcsicmp( str, named_colors[i].name ))
        {
            *argb = named_colors[i].argb;
            return TRUE;
        }
    }
    FIXME( "unsupported color %s.\n", debugstr_w(str) );
    return FALSE;
}

static BOOL scan_number( const WCHAR **str, double *value )
{
    const WCHAR *p = skip_space( *str );
    WCHAR *end;

    if (*p == ',') p = skip_space( p + 1 );
    if (!wcsnicmp( p, L"Auto", 4 ))
    {
        *value = NAN;
        *str = p + 4;
        return TRUE;
    }
    *value = wcstod( p, &end );
    if (end == p) return FALSE;
    *str = end;
    return TRUE;
}

BOOL parse_double( const WCHAR *str, double *value )
{
    return scan_number( &str, value ) && !*skip_space( str );
}

BOOL parse_thickness( const WCHAR *str, Thickness *value )
{
    double v[4];
    int count = 0;

    while (count < 4 && scan_number( &str, &v[count] )) count++;
    if (*skip_space( str )) return FALSE;

    switch (count)
    {
    case 1: value->Left = value->Top = value->Right = value->Bottom = v[0]; return TRUE;
    case 2: value->Left = value->Right = v[0]; value->Top = value->Bottom = v[1]; return TRUE;
    case 4: value->Left = v[0]; value->Top = v[1]; value->Right = v[2]; value->Bottom = v[3]; return TRUE;
    }
    return FALSE;
}

void geometry_free( struct geometry *geometry )
{
    UINT i;

    for (i = 0; i < geometry->count; i++) free( geometry->figures[i].points );
    free( geometry->figures );
    geometry->figures = NULL;
    geometry->count = geometry->size = 0;
}

static BOOL geometry_begin_figure( struct geometry *geometry )
{
    struct figure *figures;

    if (geometry->count == geometry->size)
    {
        UINT size = max( 4, geometry->size * 2 );
        if (!(figures = realloc( geometry->figures, size * sizeof(*figures) ))) return FALSE;
        geometry->figures = figures;
        geometry->size = size;
    }
    memset( &geometry->figures[geometry->count++], 0, sizeof(struct figure) );
    return TRUE;
}

static BOOL geometry_add_point( struct geometry *geometry, double x, double y )
{
    struct figure *figure;
    struct point *points;

    if (!geometry->count && !geometry_begin_figure( geometry )) return FALSE;
    figure = &geometry->figures[geometry->count - 1];
    if (figure->count == figure->size)
    {
        UINT size = max( 16, figure->size * 2 );
        if (!(points = realloc( figure->points, size * sizeof(*points) ))) return FALSE;
        figure->points = points;
        figure->size = size;
    }
    figure->points[figure->count].x = x;
    figure->points[figure->count].y = y;
    figure->count++;
    return TRUE;
}

static BOOL geometry_add_cubic( struct geometry *geometry, struct point p0, struct point p1,
                                struct point p2, struct point p3 )
{
    int i;

    for (i = 1; i <= BEZIER_SEGMENTS; i++)
    {
        double t = (double)i / BEZIER_SEGMENTS, u = 1.0 - t;
        double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        if (!geometry_add_point( geometry, a * p0.x + b * p1.x + c * p2.x + d * p3.x,
                                 a * p0.y + b * p1.y + c * p2.y + d * p3.y ))
            return FALSE;
    }
    return TRUE;
}

static BOOL geometry_add_quad( struct geometry *geometry, struct point p0, struct point p1, struct point p2 )
{
    struct point c1 = {p0.x + 2.0 / 3.0 * (p1.x - p0.x), p0.y + 2.0 / 3.0 * (p1.y - p0.y)};
    struct point c2 = {p2.x + 2.0 / 3.0 * (p1.x - p2.x), p2.y + 2.0 / 3.0 * (p1.y - p2.y)};
    return geometry_add_cubic( geometry, p0, c1, c2, p2 );
}

/* SVG/XAML endpoint arc parameterization, converted to cubic segments */
static BOOL geometry_add_arc( struct geometry *geometry, struct point p0, double rx, double ry, double angle,
                              BOOL large, BOOL sweep, struct point p1 )
{
    double phi = angle * M_PI / 180.0, cphi = cos( phi ), sphi = sin( phi );
    double dx = (p0.x - p1.x) / 2, dy = (p0.y - p1.y) / 2;
    double x1 = cphi * dx + sphi * dy, y1 = -sphi * dx + cphi * dy;
    double lambda, num, den, coef, cx1, cy1, cx, cy, theta, delta, seg_delta, t;
    int segs, i;

    rx = fabs( rx );
    ry = fabs( ry );
    if (rx == 0 || ry == 0) return geometry_add_point( geometry, p1.x, p1.y );

    lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1)
    {
        rx *= sqrt( lambda );
        ry *= sqrt( lambda );
    }

    num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    coef = den ? sqrt( max( 0.0, num / den ) ) : 0;
    if (large == sweep) coef = -coef;
    cx1 = coef * rx * y1 / ry;
    cy1 = -coef * ry * x1 / rx;
    cx = cphi * cx1 - sphi * cy1 + (p0.x + p1.x) / 2;
    cy = sphi * cx1 + cphi * cy1 + (p0.y + p1.y) / 2;

    theta = atan2( (y1 - cy1) / ry, (x1 - cx1) / rx );
    delta = atan2( (-y1 - cy1) / ry, (-x1 - cx1) / rx ) - theta;
    if (sweep && delta < 0) delta += 2 * M_PI;
    else if (!sweep && delta > 0) delta -= 2 * M_PI;

    segs = (int)ceil( fabs( delta ) / (M_PI / 2) );
    seg_delta = delta / max( segs, 1 );
    t = 4.0 / 3.0 * tan( seg_delta / 4 );

    for (i = 0; i < segs; i++)
    {
        double a0 = theta + i * seg_delta, a1 = a0 + seg_delta;
        double c0 = cos( a0 ), s0 = sin( a0 ), c1 = cos( a1 ), s1 = sin( a1 );
        struct point q0, q1, q2, q3;

#define MAP(px, py, out) out.x = cphi * rx * (px) - sphi * ry * (py) + cx; out.y = sphi * rx * (px) + cphi * ry * (py) + cy
        MAP( c0, s0, q0 );
        MAP( c0 - t * s0, s0 + t * c0, q1 );
        MAP( c1 + t * s1, s1 - t * c1, q2 );
        MAP( c1, s1, q3 );
#undef MAP
        if (!geometry_add_cubic( geometry, q0, q1, q2, q3 )) return FALSE;
    }
    return TRUE;
}

BOOL parse_path_data( const WCHAR *str, struct geometry *geometry )
{
    struct point cur = {0, 0}, start = {0, 0}, ctrl = {0, 0};
    WCHAR cmd = 0, last = 0;
    const WCHAR *p = skip_space( str );
    double v[7];
    UINT i;

    geometry_free( geometry );
    geometry->nonzero = FALSE;

    if (*p == 'F')
    {
        geometry->nonzero = p[1] == '1';
        p = skip_space( p + 2 );
    }

    while (*(p = skip_space( p )))
    {
        BOOL rel;
        int n;

        if (iswalpha( *p ) && *p != 'e' && *p != 'E')
        {
            cmd = *p++;
            if (cmd == 'Z' || cmd == 'z')
            {
                cur = start;
                last = cmd;
                continue;
            }
        }
        else if (!cmd || cmd == 'Z' || cmd == 'z')
        {
            WARN( "unexpected path data at %s.\n", debugstr_w(p) );
            return FALSE;
        }

        rel = iswlower( cmd );
        switch (towupper( cmd ))
        {
        case 'M': case 'L': case 'T': n = 2; break;
        case 'H': case 'V': n = 1; break;
        case 'C': n = 6; break;
        case 'S': case 'Q': n = 4; break;
        case 'A': n = 7; break;
        default:
            FIXME( "unsupported path command %c.\n", (char)cmd );
            return FALSE;
        }
        for (i = 0; i < n; i++)
            if (!scan_number( &p, &v[i] )) return FALSE;

        switch (towupper( cmd ))
        {
        case 'M':
            if (rel) { v[0] += cur.x; v[1] += cur.y; }
            if (!geometry_begin_figure( geometry ) || !geometry_add_point( geometry, v[0], v[1] )) return FALSE;
            cur.x = start.x = v[0];
            cur.y = start.y = v[1];
            cmd = rel ? 'l' : 'L'; /* further pairs are line segments */
            break;
        case 'L':
            if (rel) { v[0] += cur.x; v[1] += cur.y; }
            cur.x = v[0]; cur.y = v[1];
            if (!geometry_add_point( geometry, cur.x, cur.y )) return FALSE;
            break;
        case 'H':
            cur.x = rel ? cur.x + v[0] : v[0];
            if (!geometry_add_point( geometry, cur.x, cur.y )) return FALSE;
            break;
        case 'V':
            cur.y = rel ? cur.y + v[0] : v[0];
            if (!geometry_add_point( geometry, cur.x, cur.y )) return FALSE;
            break;
        case 'C':
        case 'S':
        {
            struct point c1, c2, end;
            int o = towupper( cmd ) == 'S' ? -2 : 0;

            if (o)
            {
                WCHAR l = towupper( last );
                c1 = (l == 'C' || l == 'S') ? (struct point){2 * cur.x - ctrl.x, 2 * cur.y - ctrl.y} : cur;
            }
            else
            {
                c1.x = v[0] + (rel ? cur.x : 0);
                c1.y = v[1] + (rel ? cur.y : 0);
            }
            c2.x = v[2 + o] + (rel ? cur.x : 0);
            c2.y = v[3 + o] + (rel ? cur.y : 0);
            end.x = v[4 + o] + (rel ? cur.x : 0);
            end.y = v[5 + o] + (rel ? cur.y : 0);
            if (!geometry_add_cubic( geometry, cur, c1, c2, end )) return FALSE;
            ctrl = c2;
            cur = end;
            break;
        }
        case 'Q':
        case 'T':
        {
            struct point c, end;

            if (towupper( cmd ) == 'T')
            {
                WCHAR l = towupper( last );
                c = (l == 'Q' || l == 'T') ? (struct point){2 * cur.x - ctrl.x, 2 * cur.y - ctrl.y} : cur;
                end.x = v[0] + (rel ? cur.x : 0);
                end.y = v[1] + (rel ? cur.y : 0);
            }
            else
            {
                c.x = v[0] + (rel ? cur.x : 0);
                c.y = v[1] + (rel ? cur.y : 0);
                end.x = v[2] + (rel ? cur.x : 0);
                end.y = v[3] + (rel ? cur.y : 0);
            }
            if (!geometry_add_quad( geometry, cur, c, end )) return FALSE;
            ctrl = c;
            cur = end;
            break;
        }
        case 'A':
        {
            struct point end = {v[5] + (rel ? cur.x : 0), v[6] + (rel ? cur.y : 0)};
            if (!geometry_add_arc( geometry, cur, v[0], v[1], v[2], v[3] != 0, v[4] != 0, end )) return FALSE;
            cur = end;
            break;
        }
        }
        last = cmd;
    }

    geometry->min_x = geometry->min_y = INFINITY;
    geometry->max_x = geometry->max_y = -INFINITY;
    for (i = 0; i < geometry->count; i++)
    {
        struct figure *figure = &geometry->figures[i];
        UINT j;

        for (j = 0; j < figure->count; j++)
        {
            geometry->min_x = min( geometry->min_x, figure->points[j].x );
            geometry->min_y = min( geometry->min_y, figure->points[j].y );
            geometry->max_x = max( geometry->max_x, figure->points[j].x );
            geometry->max_y = max( geometry->max_y, figure->points[j].y );
        }
    }
    if (geometry->min_x > geometry->max_x) geometry->min_x = geometry->min_y = geometry->max_x = geometry->max_y = 0;
    return TRUE;
}
