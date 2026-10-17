/* WinRT Windows.UI.Xaml Implementation
 *
 * Copyright (C) 2025 Mohamad Al-Jaf
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

#ifndef __WINE_WINDOWS_UI_XAML_PRIVATE_H
#define __WINE_WINDOWS_UI_XAML_PRIVATE_H

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"

#include "activation.h"
#include "dxgi.h"

#include "wine/debug.h"
#include "wine/list.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_UI
#include "windows.ui.h"
#define WIDL_using_Windows_UI_Core
#include "windows.ui.core.h"
#define WIDL_using_Windows_UI_Xaml
#define WIDL_using_Windows_UI_Xaml_Controls
#define WIDL_using_Windows_UI_Xaml_Controls_Primitives
#define WIDL_using_Windows_UI_Xaml_Hosting
#define WIDL_using_Windows_UI_Xaml_Markup
#include "windows.ui.xaml.h"
#include "xaml_abi.h"
#include "windows.ui.xaml.hosting.desktopwindowxamlsource.h"

extern IActivationFactory *color_helper_factory;
extern IActivationFactory *xaml_manager_factory;
extern IActivationFactory *xaml_source_factory;
extern IActivationFactory *xaml_reader_factory;

enum element_kind
{
    ELEMENT_UNKNOWN,
    ELEMENT_GRID,
    ELEMENT_STACKPANEL,
    ELEMENT_BORDER,
    ELEMENT_TEXTBLOCK,
    ELEMENT_PROGRESSBAR,
    ELEMENT_SWAPCHAINPANEL,
    ELEMENT_VIEWBOX,
    ELEMENT_PATH,
    ELEMENT_RECTANGLE,
};

/* Windows.UI.Xaml enum values */
enum { HALIGN_LEFT, HALIGN_CENTER, HALIGN_RIGHT, HALIGN_STRETCH };
enum { VALIGN_TOP, VALIGN_CENTER, VALIGN_BOTTOM, VALIGN_STRETCH };
enum { STRETCH_NONE, STRETCH_FILL, STRETCH_UNIFORM, STRETCH_UNIFORM_TO_FILL };
enum { TEXTALIGN_CENTER, TEXTALIGN_LEFT, TEXTALIGN_RIGHT, TEXTALIGN_JUSTIFY };
enum { ORIENTATION_VERTICAL, ORIENTATION_HORIZONTAL };

struct brush
{
    BOOL set;
    DWORD argb;
};

struct point { double x, y; };

struct figure
{
    struct point *points;
    UINT count, size;
};

struct geometry
{
    struct figure *figures;
    UINT count, size;
    BOOL nonzero;
    double min_x, min_y, max_x, max_y;
};

/* maps local DIPs to device pixels: dev = local * s + t */
struct xform { double sx, sy, tx, ty; };

struct element
{
    IFrameworkElement IFrameworkElement_iface;
    IUIElement IUIElement_iface;
    IDependencyObject IDependencyObject_iface;
    ITextBlock ITextBlock_iface;
    IRangeBase IRangeBase_iface;
    IProgressBar IProgressBar_iface;
    ISwapChainPanel ISwapChainPanel_iface;
    ISwapChainPanelNative ISwapChainPanelNative_iface;
    LONG ref;

    enum element_kind kind;
    WCHAR *type_name;
    struct element *parent;
    struct list children;
    struct list entry;
    struct xaml_source *host;
    WCHAR *name;

    double width, height, min_width, min_height, max_width, max_height;
    Thickness margin, padding;
    INT32 halign, valign;
    INT32 visibility;
    double opacity;
    double corner_radius;
    struct brush background, foreground, fill, stroke;
    double stroke_thickness;

    WCHAR *text;
    double font_size;
    UINT16 font_weight;
    INT32 text_alignment;
    INT32 orientation;
    double minimum, maximum, value;
    boolean indeterminate;
    INT32 stretch;
    struct geometry geometry;
    IDXGISwapChain *swapchain;

    /* layout results */
    double desired_w, desired_h;
    double actual_w, actual_h;
    double x, y; /* local DIPs of the arranged box */
    struct xform xf;
    struct xform geometry_xf; /* geometry units to local DIPs */
};

struct xaml_source
{
    IDesktopWindowXamlSource IDesktopWindowXamlSource_iface;
    IClosable IClosable_iface;
    IDesktopWindowXamlSourceNative2 IDesktopWindowXamlSourceNative2_iface;
    LONG ref;

    HWND hwnd;
    struct element *content;
    DWORD start_ticks;
    BOOL animating;
};

extern CRITICAL_SECTION xaml_cs;

extern struct element *element_create( const WCHAR *type_name );
extern struct element *unsafe_impl_from_IInspectable( IInspectable *iface );
extern void element_add_child( struct element *parent, struct element *child );
extern struct element *element_find_name( struct element *root, const WCHAR *name );
extern void element_set_host( struct element *element, struct xaml_source *host );
extern void element_invalidate( struct element *element );
extern BOOL element_needs_animation( struct element *element );

extern BOOL parse_color( const WCHAR *str, DWORD *argb );
extern BOOL parse_double( const WCHAR *str, double *value );
extern BOOL parse_thickness( const WCHAR *str, Thickness *value );
extern BOOL parse_path_data( const WCHAR *str, struct geometry *geometry );
extern void geometry_free( struct geometry *geometry );

extern HRESULT xaml_parse( const WCHAR *xaml, struct element **root );

extern void render_content( struct element *root, HDC hdc, int width, int height, double dpi_scale, DWORD time );

#define DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from, iface_mem, expr )             \
    static inline impl_type *impl_from( iface_type *iface )                                        \
    {                                                                                              \
        return CONTAINING_RECORD( iface, impl_type, iface_mem );                                   \
    }                                                                                              \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_QueryInterface( (IInspectable *)(expr), iid, out );                    \
    }                                                                                              \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface )                                          \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_AddRef( (IInspectable *)(expr) );                                      \
    }                                                                                              \
    static ULONG WINAPI pfx##_Release( iface_type *iface )                                         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_Release( (IInspectable *)(expr) );                                     \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetIids( (IInspectable *)(expr), iid_count, iids );                    \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )      \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetRuntimeClassName( (IInspectable *)(expr), class_name );             \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *trust_level )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetTrustLevel( (IInspectable *)(expr), trust_level );                  \
    }
#define DEFINE_IINSPECTABLE( pfx, iface_type, impl_type, base_iface )                              \
    DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from_##iface_type, iface_type##_iface, &impl->base_iface )

#endif
