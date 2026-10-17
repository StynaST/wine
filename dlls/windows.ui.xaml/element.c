/* WinRT Windows.UI.Xaml element tree
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

static const struct
{
    const WCHAR *name;
    const WCHAR *class_name;
    enum element_kind kind;
}
element_types[] =
{
    {L"Grid",           L"Windows.UI.Xaml.Controls.Grid",           ELEMENT_GRID},
    {L"StackPanel",     L"Windows.UI.Xaml.Controls.StackPanel",     ELEMENT_STACKPANEL},
    {L"Border",         L"Windows.UI.Xaml.Controls.Border",         ELEMENT_BORDER},
    {L"TextBlock",      L"Windows.UI.Xaml.Controls.TextBlock",      ELEMENT_TEXTBLOCK},
    {L"ProgressBar",    L"Windows.UI.Xaml.Controls.ProgressBar",    ELEMENT_PROGRESSBAR},
    {L"SwapChainPanel", L"Windows.UI.Xaml.Controls.SwapChainPanel", ELEMENT_SWAPCHAINPANEL},
    {L"Viewbox",        L"Windows.UI.Xaml.Controls.Viewbox",        ELEMENT_VIEWBOX},
    {L"Path",           L"Windows.UI.Xaml.Shapes.Path",             ELEMENT_PATH},
    {L"Rectangle",      L"Windows.UI.Xaml.Shapes.Rectangle",        ELEMENT_RECTANGLE},
};

static inline const char *debugstr_double( double value )
{
    return wine_dbg_sprintf( "%g", value );
}

static inline struct element *impl_from_IFrameworkElement( IFrameworkElement *iface )
{
    return CONTAINING_RECORD( iface, struct element, IFrameworkElement_iface );
}

static HRESULT WINAPI framework_QueryInterface( IFrameworkElement *iface, REFIID iid, void **out )
{
    struct element *impl = impl_from_IFrameworkElement( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IFrameworkElement ))
        *out = &impl->IFrameworkElement_iface;
    else if (IsEqualGUID( iid, &IID_IUIElement ))
        *out = &impl->IUIElement_iface;
    else if (IsEqualGUID( iid, &IID_IDependencyObject ))
        *out = &impl->IDependencyObject_iface;
    else if (impl->kind == ELEMENT_TEXTBLOCK && IsEqualGUID( iid, &IID_ITextBlock ))
        *out = &impl->ITextBlock_iface;
    else if (impl->kind == ELEMENT_PROGRESSBAR && IsEqualGUID( iid, &IID_IRangeBase ))
        *out = &impl->IRangeBase_iface;
    else if (impl->kind == ELEMENT_PROGRESSBAR && IsEqualGUID( iid, &IID_IProgressBar ))
        *out = &impl->IProgressBar_iface;
    else if (impl->kind == ELEMENT_SWAPCHAINPANEL && IsEqualGUID( iid, &IID_ISwapChainPanel ))
        *out = &impl->ISwapChainPanel_iface;
    else if (impl->kind == ELEMENT_SWAPCHAINPANEL && IsEqualGUID( iid, &IID_ISwapChainPanelNative ))
        *out = &impl->ISwapChainPanelNative_iface;
    else
    {
        *out = NULL;
        WARN( "%s not implemented by %s, returning E_NOINTERFACE.\n", debugstr_guid( iid ), debugstr_w( impl->type_name ) );
        return E_NOINTERFACE;
    }

    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI framework_AddRef( IFrameworkElement *iface )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static void element_destroy( struct element *impl )
{
    struct element *child, *next;

    LIST_FOR_EACH_ENTRY_SAFE( child, next, &impl->children, struct element, entry )
    {
        list_remove( &child->entry );
        child->parent = NULL;
        IFrameworkElement_Release( &child->IFrameworkElement_iface );
    }
    if (impl->swapchain) IDXGISwapChain_Release( impl->swapchain );
    geometry_free( &impl->geometry );
    free( impl->type_name );
    free( impl->name );
    free( impl->text );
    free( impl );
}

static ULONG WINAPI framework_Release( IFrameworkElement *iface )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        EnterCriticalSection( &xaml_cs );
        element_destroy( impl );
        LeaveCriticalSection( &xaml_cs );
    }
    return ref;
}

static HRESULT WINAPI framework_GetIids( IFrameworkElement *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_GetRuntimeClassName( IFrameworkElement *iface, HSTRING *class_name )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    unsigned int i;

    TRACE( "iface %p, class_name %p.\n", iface, class_name );

    for (i = 0; i < ARRAY_SIZE(element_types); i++)
        if (element_types[i].kind == impl->kind && impl->kind != ELEMENT_UNKNOWN)
            return WindowsCreateString( element_types[i].class_name, wcslen( element_types[i].class_name ), class_name );
    return WindowsCreateString( L"Windows.UI.Xaml.FrameworkElement", wcslen( L"Windows.UI.Xaml.FrameworkElement" ), class_name );
}

static HRESULT WINAPI framework_GetTrustLevel( IFrameworkElement *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    *trust_level = BaseTrust;
    return S_OK;
}

DEFINE_IINSPECTABLE( uielement, IUIElement, struct element, IFrameworkElement_iface )

static HRESULT WINAPI uielement_get_DesiredSize( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, __x_ABI_CWindows_CFoundation_CSize *value )
{
    struct element *impl = impl_from_IUIElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    value->Width = impl->desired_w;
    value->Height = impl->desired_h;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI uielement_get_AllowDrop( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, boolean *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_put_AllowDrop( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, boolean value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_Opacity( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, DOUBLE *value )
{
    struct element *impl = impl_from_IUIElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->opacity;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI uielement_put_Opacity( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, DOUBLE value )
{
    struct element *impl = impl_from_IUIElement( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_double( value ) );
    EnterCriticalSection( &xaml_cs );
    impl->opacity = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI uielement_get_Clip( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_put_Clip( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_RenderTransform( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_put_RenderTransform( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_Projection( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_put_Projection( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_RenderTransformOrigin( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, __x_ABI_CWindows_CFoundation_CPoint *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_put_RenderTransformOrigin( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, __x_ABI_CWindows_CFoundation_CPoint value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_IsHitTestVisible( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, boolean *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_put_IsHitTestVisible( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, boolean value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_Visibility( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, INT32 *value )
{
    struct element *impl = impl_from_IUIElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->visibility;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI uielement_put_Visibility( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, INT32 value )
{
    struct element *impl = impl_from_IUIElement( iface );
    TRACE( "iface %p, value %d.\n", iface, value );
    EnterCriticalSection( &xaml_cs );
    impl->visibility = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI uielement_get_RenderSize( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, __x_ABI_CWindows_CFoundation_CSize *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_UseLayoutRounding( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, boolean *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_put_UseLayoutRounding( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, boolean value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_Transitions( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_put_Transitions( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_CacheMode( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_put_CacheMode( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_IsTapEnabled( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, boolean *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_put_IsTapEnabled( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, boolean value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_IsDoubleTapEnabled( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, boolean *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_put_IsDoubleTapEnabled( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, boolean value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_IsRightTapEnabled( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, boolean *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_put_IsRightTapEnabled( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, boolean value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_IsHoldingEnabled( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, boolean *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_put_IsHoldingEnabled( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, boolean value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_ManipulationMode( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, UINT32 *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_put_ManipulationMode( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, UINT32 value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_get_PointerCaptures( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_KeyUp( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_KeyUp( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_KeyDown( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_KeyDown( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_GotFocus( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_GotFocus( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_LostFocus( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_LostFocus( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_DragEnter( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_DragEnter( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_DragLeave( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_DragLeave( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_DragOver( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_DragOver( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_Drop( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_Drop( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_PointerPressed( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_PointerPressed( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_PointerMoved( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_PointerMoved( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_PointerReleased( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_PointerReleased( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_PointerEntered( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_PointerEntered( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_PointerExited( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_PointerExited( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_PointerCaptureLost( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_PointerCaptureLost( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_PointerCanceled( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_PointerCanceled( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_PointerWheelChanged( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_PointerWheelChanged( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_Tapped( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_Tapped( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_DoubleTapped( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_DoubleTapped( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_Holding( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_Holding( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_RightTapped( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_RightTapped( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_ManipulationStarting( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_ManipulationStarting( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_ManipulationInertiaStarting( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_ManipulationInertiaStarting( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_ManipulationStarted( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_ManipulationStarted( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_ManipulationDelta( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_ManipulationDelta( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_add_ManipulationCompleted( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_remove_ManipulationCompleted( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_Measure( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, __x_ABI_CWindows_CFoundation_CSize availableSize )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_Arrange( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, __x_ABI_CWindows_CFoundation_CRect finalRect )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_CapturePointer( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * value, boolean *result )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_ReleasePointerCapture( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_ReleasePointerCaptures( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_AddHandler( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * routedEvent, IInspectable * handler, boolean handledEventsToo )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_RemoveHandler( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * routedEvent, IInspectable * handler )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_TransformToVisual( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface, IInspectable * visual, IInspectable * *result )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_InvalidateMeasure( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_InvalidateArrange( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI uielement_UpdateLayout( __x_ABI_CWindows_CUI_CXaml_CIUIElement *iface )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static const struct __x_ABI_CWindows_CUI_CXaml_CIUIElementVtbl uielement_vtbl =
{
    uielement_QueryInterface,
    uielement_AddRef,
    uielement_Release,
    /* IInspectable methods */
    uielement_GetIids,
    uielement_GetRuntimeClassName,
    uielement_GetTrustLevel,
    /* IUIElement methods */
    uielement_get_DesiredSize,
    uielement_get_AllowDrop,
    uielement_put_AllowDrop,
    uielement_get_Opacity,
    uielement_put_Opacity,
    uielement_get_Clip,
    uielement_put_Clip,
    uielement_get_RenderTransform,
    uielement_put_RenderTransform,
    uielement_get_Projection,
    uielement_put_Projection,
    uielement_get_RenderTransformOrigin,
    uielement_put_RenderTransformOrigin,
    uielement_get_IsHitTestVisible,
    uielement_put_IsHitTestVisible,
    uielement_get_Visibility,
    uielement_put_Visibility,
    uielement_get_RenderSize,
    uielement_get_UseLayoutRounding,
    uielement_put_UseLayoutRounding,
    uielement_get_Transitions,
    uielement_put_Transitions,
    uielement_get_CacheMode,
    uielement_put_CacheMode,
    uielement_get_IsTapEnabled,
    uielement_put_IsTapEnabled,
    uielement_get_IsDoubleTapEnabled,
    uielement_put_IsDoubleTapEnabled,
    uielement_get_IsRightTapEnabled,
    uielement_put_IsRightTapEnabled,
    uielement_get_IsHoldingEnabled,
    uielement_put_IsHoldingEnabled,
    uielement_get_ManipulationMode,
    uielement_put_ManipulationMode,
    uielement_get_PointerCaptures,
    uielement_add_KeyUp,
    uielement_remove_KeyUp,
    uielement_add_KeyDown,
    uielement_remove_KeyDown,
    uielement_add_GotFocus,
    uielement_remove_GotFocus,
    uielement_add_LostFocus,
    uielement_remove_LostFocus,
    uielement_add_DragEnter,
    uielement_remove_DragEnter,
    uielement_add_DragLeave,
    uielement_remove_DragLeave,
    uielement_add_DragOver,
    uielement_remove_DragOver,
    uielement_add_Drop,
    uielement_remove_Drop,
    uielement_add_PointerPressed,
    uielement_remove_PointerPressed,
    uielement_add_PointerMoved,
    uielement_remove_PointerMoved,
    uielement_add_PointerReleased,
    uielement_remove_PointerReleased,
    uielement_add_PointerEntered,
    uielement_remove_PointerEntered,
    uielement_add_PointerExited,
    uielement_remove_PointerExited,
    uielement_add_PointerCaptureLost,
    uielement_remove_PointerCaptureLost,
    uielement_add_PointerCanceled,
    uielement_remove_PointerCanceled,
    uielement_add_PointerWheelChanged,
    uielement_remove_PointerWheelChanged,
    uielement_add_Tapped,
    uielement_remove_Tapped,
    uielement_add_DoubleTapped,
    uielement_remove_DoubleTapped,
    uielement_add_Holding,
    uielement_remove_Holding,
    uielement_add_RightTapped,
    uielement_remove_RightTapped,
    uielement_add_ManipulationStarting,
    uielement_remove_ManipulationStarting,
    uielement_add_ManipulationInertiaStarting,
    uielement_remove_ManipulationInertiaStarting,
    uielement_add_ManipulationStarted,
    uielement_remove_ManipulationStarted,
    uielement_add_ManipulationDelta,
    uielement_remove_ManipulationDelta,
    uielement_add_ManipulationCompleted,
    uielement_remove_ManipulationCompleted,
    uielement_Measure,
    uielement_Arrange,
    uielement_CapturePointer,
    uielement_ReleasePointerCapture,
    uielement_ReleasePointerCaptures,
    uielement_AddHandler,
    uielement_RemoveHandler,
    uielement_TransformToVisual,
    uielement_InvalidateMeasure,
    uielement_InvalidateArrange,
    uielement_UpdateLayout,
};

static HRESULT WINAPI framework_get_Triggers( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_get_Resources( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_put_Resources( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_get_Tag( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_put_Tag( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_get_Language( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, HSTRING *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_put_Language( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, HSTRING value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_get_ActualWidth( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, DOUBLE *value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->actual_w;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI framework_get_ActualHeight( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, DOUBLE *value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->actual_h;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI framework_get_Width( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, DOUBLE *value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->width;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI framework_put_Width( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, DOUBLE value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_double( value ) );
    EnterCriticalSection( &xaml_cs );
    impl->width = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI framework_get_Height( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, DOUBLE *value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->height;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI framework_put_Height( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, DOUBLE value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_double( value ) );
    EnterCriticalSection( &xaml_cs );
    impl->height = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI framework_get_MinWidth( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, DOUBLE *value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->min_width;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI framework_put_MinWidth( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, DOUBLE value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_double( value ) );
    EnterCriticalSection( &xaml_cs );
    impl->min_width = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI framework_get_MaxWidth( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, DOUBLE *value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->max_width;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI framework_put_MaxWidth( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, DOUBLE value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_double( value ) );
    EnterCriticalSection( &xaml_cs );
    impl->max_width = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI framework_get_MinHeight( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, DOUBLE *value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->min_height;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI framework_put_MinHeight( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, DOUBLE value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_double( value ) );
    EnterCriticalSection( &xaml_cs );
    impl->min_height = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI framework_get_MaxHeight( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, DOUBLE *value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->max_height;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI framework_put_MaxHeight( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, DOUBLE value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_double( value ) );
    EnterCriticalSection( &xaml_cs );
    impl->max_height = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI framework_get_HorizontalAlignment( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, INT32 *value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->halign;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI framework_put_HorizontalAlignment( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, INT32 value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %d.\n", iface, value );
    EnterCriticalSection( &xaml_cs );
    impl->halign = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI framework_get_VerticalAlignment( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, INT32 *value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->valign;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI framework_put_VerticalAlignment( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, INT32 value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %d.\n", iface, value );
    EnterCriticalSection( &xaml_cs );
    impl->valign = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI framework_get_Margin( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, __x_ABI_CWindows_CUI_CXaml_CThickness *value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->margin;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI framework_put_Margin( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, __x_ABI_CWindows_CUI_CXaml_CThickness value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %p.\n", iface, &value );
    EnterCriticalSection( &xaml_cs );
    impl->margin = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI framework_get_Name( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, HSTRING *value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    HRESULT hr;
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    hr = WindowsCreateString( impl->name, impl->name ? wcslen( impl->name ) : 0, value );
    LeaveCriticalSection( &xaml_cs );
    return hr;
}

static HRESULT WINAPI framework_put_Name( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, HSTRING value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    WCHAR *name;
    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    if (!(name = wcsdup( WindowsGetStringRawBuffer( value, NULL ) ))) return E_OUTOFMEMORY;
    EnterCriticalSection( &xaml_cs );
    free( impl->name );
    impl->name = name;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI framework_get_BaseUri( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_get_DataContext( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_put_DataContext( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_get_Style( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_put_Style( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_get_Parent( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * *value )
{
    struct element *impl = impl_from_IFrameworkElement( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    if ((*value = impl->parent ? (IInspectable *)&impl->parent->IFrameworkElement_iface : NULL))
        IInspectable_AddRef( *value );
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI framework_get_FlowDirection( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, INT32 *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_put_FlowDirection( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, INT32 value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_add_Loaded( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_remove_Loaded( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_add_Unloaded( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_remove_Unloaded( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_add_SizeChanged( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_remove_SizeChanged( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_add_LayoutUpdated( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_remove_LayoutUpdated( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI framework_FindName( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, HSTRING name, IInspectable * *result )
{
    struct element *impl = impl_from_IFrameworkElement( iface ), *root, *found;
    TRACE( "iface %p, name %s, result %p.\n", iface, debugstr_hstring( name ), result );
    if (!result) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    for (root = impl; root->parent; root = root->parent) ;
    if ((found = element_find_name( root, WindowsGetStringRawBuffer( name, NULL ) )))
    {
        *result = (IInspectable *)&found->IFrameworkElement_iface;
        IInspectable_AddRef( *result );
    }
    else *result = NULL;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI framework_SetBinding( __x_ABI_CWindows_CUI_CXaml_CIFrameworkElement *iface, IInspectable * dp, IInspectable * binding )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static const struct __x_ABI_CWindows_CUI_CXaml_CIFrameworkElementVtbl framework_vtbl =
{
    framework_QueryInterface,
    framework_AddRef,
    framework_Release,
    /* IInspectable methods */
    framework_GetIids,
    framework_GetRuntimeClassName,
    framework_GetTrustLevel,
    /* IFrameworkElement methods */
    framework_get_Triggers,
    framework_get_Resources,
    framework_put_Resources,
    framework_get_Tag,
    framework_put_Tag,
    framework_get_Language,
    framework_put_Language,
    framework_get_ActualWidth,
    framework_get_ActualHeight,
    framework_get_Width,
    framework_put_Width,
    framework_get_Height,
    framework_put_Height,
    framework_get_MinWidth,
    framework_put_MinWidth,
    framework_get_MaxWidth,
    framework_put_MaxWidth,
    framework_get_MinHeight,
    framework_put_MinHeight,
    framework_get_MaxHeight,
    framework_put_MaxHeight,
    framework_get_HorizontalAlignment,
    framework_put_HorizontalAlignment,
    framework_get_VerticalAlignment,
    framework_put_VerticalAlignment,
    framework_get_Margin,
    framework_put_Margin,
    framework_get_Name,
    framework_put_Name,
    framework_get_BaseUri,
    framework_get_DataContext,
    framework_put_DataContext,
    framework_get_Style,
    framework_put_Style,
    framework_get_Parent,
    framework_get_FlowDirection,
    framework_put_FlowDirection,
    framework_add_Loaded,
    framework_remove_Loaded,
    framework_add_Unloaded,
    framework_remove_Unloaded,
    framework_add_SizeChanged,
    framework_remove_SizeChanged,
    framework_add_LayoutUpdated,
    framework_remove_LayoutUpdated,
    framework_FindName,
    framework_SetBinding,
};

DEFINE_IINSPECTABLE( textblock, ITextBlock, struct element, IFrameworkElement_iface )

static HRESULT WINAPI textblock_get_FontSize( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, DOUBLE *value )
{
    struct element *impl = impl_from_ITextBlock( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->font_size;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI textblock_put_FontSize( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, DOUBLE value )
{
    struct element *impl = impl_from_ITextBlock( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_double( value ) );
    EnterCriticalSection( &xaml_cs );
    impl->font_size = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI textblock_get_FontFamily( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_put_FontFamily( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, IInspectable * value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_FontWeight( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, __x_ABI_CWindows_CUI_CText_CFontWeight *value )
{
    struct element *impl = impl_from_ITextBlock( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    value->Weight = impl->font_weight;
    return S_OK;
}

static HRESULT WINAPI textblock_put_FontWeight( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, __x_ABI_CWindows_CUI_CText_CFontWeight value )
{
    struct element *impl = impl_from_ITextBlock( iface );
    TRACE( "iface %p, value %u.\n", iface, value.Weight );
    impl->font_weight = value.Weight;
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI textblock_get_FontStyle( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_put_FontStyle( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_FontStretch( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_put_FontStretch( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_CharacterSpacing( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_put_CharacterSpacing( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_Foreground( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_put_Foreground( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, IInspectable * value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_TextWrapping( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_put_TextWrapping( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_TextTrimming( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_put_TextTrimming( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_TextAlignment( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 *value )
{
    struct element *impl = impl_from_ITextBlock( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->text_alignment;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI textblock_put_TextAlignment( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 value )
{
    struct element *impl = impl_from_ITextBlock( iface );
    TRACE( "iface %p, value %d.\n", iface, value );
    EnterCriticalSection( &xaml_cs );
    impl->text_alignment = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI textblock_get_Text( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, HSTRING *value )
{
    struct element *impl = impl_from_ITextBlock( iface );
    HRESULT hr;
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    hr = WindowsCreateString( impl->text, impl->text ? wcslen( impl->text ) : 0, value );
    LeaveCriticalSection( &xaml_cs );
    return hr;
}

static HRESULT WINAPI textblock_put_Text( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, HSTRING value )
{
    struct element *impl = impl_from_ITextBlock( iface );
    WCHAR *text;
    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    if (!(text = wcsdup( WindowsGetStringRawBuffer( value, NULL ) ))) return E_OUTOFMEMORY;
    EnterCriticalSection( &xaml_cs );
    free( impl->text );
    impl->text = text;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI textblock_get_Inlines( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_Padding( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, __x_ABI_CWindows_CUI_CXaml_CThickness *value )
{
    struct element *impl = impl_from_ITextBlock( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->padding;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI textblock_put_Padding( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, __x_ABI_CWindows_CUI_CXaml_CThickness value )
{
    struct element *impl = impl_from_ITextBlock( iface );
    TRACE( "iface %p, value %p.\n", iface, &value );
    EnterCriticalSection( &xaml_cs );
    impl->padding = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI textblock_get_LineHeight( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, DOUBLE *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_put_LineHeight( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, DOUBLE value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_LineStackingStrategy( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_put_LineStackingStrategy( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_IsTextSelectionEnabled( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, boolean *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_put_IsTextSelectionEnabled( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, boolean value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_SelectedText( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, HSTRING *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_ContentStart( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_ContentEnd( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_SelectionStart( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_SelectionEnd( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_get_BaselineOffset( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, DOUBLE *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_add_SelectionChanged( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_remove_SelectionChanged( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_add_ContextMenuOpening( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_remove_ContextMenuOpening( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_SelectAll( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_Select( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, IInspectable * start, IInspectable * end )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI textblock_Focus( __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlock *iface, INT32 value, boolean *result )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static const struct __x_ABI_CWindows_CUI_CXaml_CControls_CITextBlockVtbl textblock_vtbl =
{
    textblock_QueryInterface,
    textblock_AddRef,
    textblock_Release,
    /* IInspectable methods */
    textblock_GetIids,
    textblock_GetRuntimeClassName,
    textblock_GetTrustLevel,
    /* ITextBlock methods */
    textblock_get_FontSize,
    textblock_put_FontSize,
    textblock_get_FontFamily,
    textblock_put_FontFamily,
    textblock_get_FontWeight,
    textblock_put_FontWeight,
    textblock_get_FontStyle,
    textblock_put_FontStyle,
    textblock_get_FontStretch,
    textblock_put_FontStretch,
    textblock_get_CharacterSpacing,
    textblock_put_CharacterSpacing,
    textblock_get_Foreground,
    textblock_put_Foreground,
    textblock_get_TextWrapping,
    textblock_put_TextWrapping,
    textblock_get_TextTrimming,
    textblock_put_TextTrimming,
    textblock_get_TextAlignment,
    textblock_put_TextAlignment,
    textblock_get_Text,
    textblock_put_Text,
    textblock_get_Inlines,
    textblock_get_Padding,
    textblock_put_Padding,
    textblock_get_LineHeight,
    textblock_put_LineHeight,
    textblock_get_LineStackingStrategy,
    textblock_put_LineStackingStrategy,
    textblock_get_IsTextSelectionEnabled,
    textblock_put_IsTextSelectionEnabled,
    textblock_get_SelectedText,
    textblock_get_ContentStart,
    textblock_get_ContentEnd,
    textblock_get_SelectionStart,
    textblock_get_SelectionEnd,
    textblock_get_BaselineOffset,
    textblock_add_SelectionChanged,
    textblock_remove_SelectionChanged,
    textblock_add_ContextMenuOpening,
    textblock_remove_ContextMenuOpening,
    textblock_SelectAll,
    textblock_Select,
    textblock_Focus,
};

DEFINE_IINSPECTABLE( rangebase, IRangeBase, struct element, IFrameworkElement_iface )

static HRESULT WINAPI rangebase_get_Minimum( __x_ABI_CWindows_CUI_CXaml_CControls_CPrimitives_CIRangeBase *iface, DOUBLE *value )
{
    struct element *impl = impl_from_IRangeBase( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->minimum;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI rangebase_put_Minimum( __x_ABI_CWindows_CUI_CXaml_CControls_CPrimitives_CIRangeBase *iface, DOUBLE value )
{
    struct element *impl = impl_from_IRangeBase( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_double( value ) );
    EnterCriticalSection( &xaml_cs );
    impl->minimum = value;
    if (impl->maximum < value) impl->maximum = value;
    if (impl->value < value) impl->value = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI rangebase_get_Maximum( __x_ABI_CWindows_CUI_CXaml_CControls_CPrimitives_CIRangeBase *iface, DOUBLE *value )
{
    struct element *impl = impl_from_IRangeBase( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->maximum;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI rangebase_put_Maximum( __x_ABI_CWindows_CUI_CXaml_CControls_CPrimitives_CIRangeBase *iface, DOUBLE value )
{
    struct element *impl = impl_from_IRangeBase( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_double( value ) );
    EnterCriticalSection( &xaml_cs );
    impl->maximum = max( value, impl->minimum );
    if (impl->value > impl->maximum) impl->value = impl->maximum;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI rangebase_get_SmallChange( __x_ABI_CWindows_CUI_CXaml_CControls_CPrimitives_CIRangeBase *iface, DOUBLE *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI rangebase_put_SmallChange( __x_ABI_CWindows_CUI_CXaml_CControls_CPrimitives_CIRangeBase *iface, DOUBLE value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI rangebase_get_LargeChange( __x_ABI_CWindows_CUI_CXaml_CControls_CPrimitives_CIRangeBase *iface, DOUBLE *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI rangebase_put_LargeChange( __x_ABI_CWindows_CUI_CXaml_CControls_CPrimitives_CIRangeBase *iface, DOUBLE value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI rangebase_get_Value( __x_ABI_CWindows_CUI_CXaml_CControls_CPrimitives_CIRangeBase *iface, DOUBLE *value )
{
    struct element *impl = impl_from_IRangeBase( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->value;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI rangebase_put_Value( __x_ABI_CWindows_CUI_CXaml_CControls_CPrimitives_CIRangeBase *iface, DOUBLE value )
{
    struct element *impl = impl_from_IRangeBase( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_double( value ) );
    EnterCriticalSection( &xaml_cs );
    impl->value = min( max( value, impl->minimum ), impl->maximum );
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI rangebase_add_ValueChanged( __x_ABI_CWindows_CUI_CXaml_CControls_CPrimitives_CIRangeBase *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI rangebase_remove_ValueChanged( __x_ABI_CWindows_CUI_CXaml_CControls_CPrimitives_CIRangeBase *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static const struct __x_ABI_CWindows_CUI_CXaml_CControls_CPrimitives_CIRangeBaseVtbl rangebase_vtbl =
{
    rangebase_QueryInterface,
    rangebase_AddRef,
    rangebase_Release,
    /* IInspectable methods */
    rangebase_GetIids,
    rangebase_GetRuntimeClassName,
    rangebase_GetTrustLevel,
    /* IRangeBase methods */
    rangebase_get_Minimum,
    rangebase_put_Minimum,
    rangebase_get_Maximum,
    rangebase_put_Maximum,
    rangebase_get_SmallChange,
    rangebase_put_SmallChange,
    rangebase_get_LargeChange,
    rangebase_put_LargeChange,
    rangebase_get_Value,
    rangebase_put_Value,
    rangebase_add_ValueChanged,
    rangebase_remove_ValueChanged,
};

DEFINE_IINSPECTABLE( progressbar, IProgressBar, struct element, IFrameworkElement_iface )

static HRESULT WINAPI progressbar_get_IsIndeterminate( __x_ABI_CWindows_CUI_CXaml_CControls_CIProgressBar *iface, boolean *value )
{
    struct element *impl = impl_from_IProgressBar( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    *value = impl->indeterminate;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI progressbar_put_IsIndeterminate( __x_ABI_CWindows_CUI_CXaml_CControls_CIProgressBar *iface, boolean value )
{
    struct element *impl = impl_from_IProgressBar( iface );
    TRACE( "iface %p, value %d.\n", iface, value );
    EnterCriticalSection( &xaml_cs );
    impl->indeterminate = value;
    LeaveCriticalSection( &xaml_cs );
    element_invalidate( impl );
    return S_OK;
}

static HRESULT WINAPI progressbar_get_ShowError( __x_ABI_CWindows_CUI_CXaml_CControls_CIProgressBar *iface, boolean *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI progressbar_put_ShowError( __x_ABI_CWindows_CUI_CXaml_CControls_CIProgressBar *iface, boolean value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI progressbar_get_ShowPaused( __x_ABI_CWindows_CUI_CXaml_CControls_CIProgressBar *iface, boolean *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI progressbar_put_ShowPaused( __x_ABI_CWindows_CUI_CXaml_CControls_CIProgressBar *iface, boolean value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI progressbar_get_TemplateSettings( __x_ABI_CWindows_CUI_CXaml_CControls_CIProgressBar *iface, IInspectable * *value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static const struct __x_ABI_CWindows_CUI_CXaml_CControls_CIProgressBarVtbl progressbar_vtbl =
{
    progressbar_QueryInterface,
    progressbar_AddRef,
    progressbar_Release,
    /* IInspectable methods */
    progressbar_GetIids,
    progressbar_GetRuntimeClassName,
    progressbar_GetTrustLevel,
    /* IProgressBar methods */
    progressbar_get_IsIndeterminate,
    progressbar_put_IsIndeterminate,
    progressbar_get_ShowError,
    progressbar_put_ShowError,
    progressbar_get_ShowPaused,
    progressbar_put_ShowPaused,
    progressbar_get_TemplateSettings,
};

DEFINE_IINSPECTABLE( swapchainpanel, ISwapChainPanel, struct element, IFrameworkElement_iface )

static HRESULT WINAPI swapchainpanel_get_CompositionScaleX( __x_ABI_CWindows_CUI_CXaml_CControls_CISwapChainPanel *iface, FLOAT *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = 1.0f;
    return S_OK;
}

static HRESULT WINAPI swapchainpanel_get_CompositionScaleY( __x_ABI_CWindows_CUI_CXaml_CControls_CISwapChainPanel *iface, FLOAT *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = 1.0f;
    return S_OK;
}

static HRESULT WINAPI swapchainpanel_add_CompositionScaleChanged( __x_ABI_CWindows_CUI_CXaml_CControls_CISwapChainPanel *iface, IInspectable * handler, EventRegistrationToken *token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI swapchainpanel_remove_CompositionScaleChanged( __x_ABI_CWindows_CUI_CXaml_CControls_CISwapChainPanel *iface, EventRegistrationToken token )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI swapchainpanel_CreateCoreIndependentInputSource( __x_ABI_CWindows_CUI_CXaml_CControls_CISwapChainPanel *iface, UINT32 deviceTypes, IInspectable * *result )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static const struct __x_ABI_CWindows_CUI_CXaml_CControls_CISwapChainPanelVtbl swapchainpanel_vtbl =
{
    swapchainpanel_QueryInterface,
    swapchainpanel_AddRef,
    swapchainpanel_Release,
    /* IInspectable methods */
    swapchainpanel_GetIids,
    swapchainpanel_GetRuntimeClassName,
    swapchainpanel_GetTrustLevel,
    /* ISwapChainPanel methods */
    swapchainpanel_get_CompositionScaleX,
    swapchainpanel_get_CompositionScaleY,
    swapchainpanel_add_CompositionScaleChanged,
    swapchainpanel_remove_CompositionScaleChanged,
    swapchainpanel_CreateCoreIndependentInputSource,
};

DEFINE_IINSPECTABLE( depobj, IDependencyObject, struct element, IFrameworkElement_iface )

static HRESULT WINAPI depobj_GetValue( IDependencyObject *iface, IDependencyProperty *dp, IInspectable **result )
{
    FIXME( "iface %p, dp %p, result %p stub!\n", iface, dp, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI depobj_SetValue( IDependencyObject *iface, IDependencyProperty *dp, IInspectable *value )
{
    FIXME( "iface %p, dp %p, value %p stub!\n", iface, dp, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI depobj_ClearValue( IDependencyObject *iface, IDependencyProperty *dp )
{
    FIXME( "iface %p, dp %p stub!\n", iface, dp );
    return E_NOTIMPL;
}

static HRESULT WINAPI depobj_ReadLocalValue( IDependencyObject *iface, IDependencyProperty *dp, IInspectable **result )
{
    FIXME( "iface %p, dp %p, result %p stub!\n", iface, dp, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI depobj_GetAnimationBaseValue( IDependencyObject *iface, IDependencyProperty *dp, IInspectable **result )
{
    FIXME( "iface %p, dp %p, result %p stub!\n", iface, dp, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI depobj_get_Dispatcher( IDependencyObject *iface, ICoreDispatcher **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static const struct IDependencyObjectVtbl depobj_vtbl =
{
    depobj_QueryInterface,
    depobj_AddRef,
    depobj_Release,
    /* IInspectable methods */
    depobj_GetIids,
    depobj_GetRuntimeClassName,
    depobj_GetTrustLevel,
    /* IDependencyObject methods */
    depobj_GetValue,
    depobj_SetValue,
    depobj_ClearValue,
    depobj_ReadLocalValue,
    depobj_GetAnimationBaseValue,
    depobj_get_Dispatcher,
};

static inline struct element *impl_from_ISwapChainPanelNative( ISwapChainPanelNative *iface )
{
    return CONTAINING_RECORD( iface, struct element, ISwapChainPanelNative_iface );
}

static HRESULT WINAPI swapchain_native_QueryInterface( ISwapChainPanelNative *iface, REFIID iid, void **out )
{
    struct element *impl = impl_from_ISwapChainPanelNative( iface );
    return IFrameworkElement_QueryInterface( &impl->IFrameworkElement_iface, iid, out );
}

static ULONG WINAPI swapchain_native_AddRef( ISwapChainPanelNative *iface )
{
    struct element *impl = impl_from_ISwapChainPanelNative( iface );
    return IFrameworkElement_AddRef( &impl->IFrameworkElement_iface );
}

static ULONG WINAPI swapchain_native_Release( ISwapChainPanelNative *iface )
{
    struct element *impl = impl_from_ISwapChainPanelNative( iface );
    return IFrameworkElement_Release( &impl->IFrameworkElement_iface );
}

static HRESULT WINAPI swapchain_native_SetSwapChain( ISwapChainPanelNative *iface, IDXGISwapChain *swapchain )
{
    struct element *impl = impl_from_ISwapChainPanelNative( iface );
    IDXGISwapChain *old;

    FIXME( "iface %p, swapchain %p, composition of swapchain content not implemented.\n", iface, swapchain );

    if (swapchain) IDXGISwapChain_AddRef( swapchain );
    EnterCriticalSection( &xaml_cs );
    old = impl->swapchain;
    impl->swapchain = swapchain;
    LeaveCriticalSection( &xaml_cs );
    if (old) IDXGISwapChain_Release( old );
    return S_OK;
}

static const struct ISwapChainPanelNativeVtbl swapchain_native_vtbl =
{
    swapchain_native_QueryInterface,
    swapchain_native_AddRef,
    swapchain_native_Release,
    swapchain_native_SetSwapChain,
};

struct element *unsafe_impl_from_IInspectable( IInspectable *iface )
{
    IFrameworkElement *element;
    struct element *impl;

    if (!iface) return NULL;
    if (FAILED(IInspectable_QueryInterface( iface, &IID_IFrameworkElement, (void **)&element ))) return NULL;
    if (element->lpVtbl != &framework_vtbl)
    {
        IFrameworkElement_Release( element );
        return NULL;
    }
    impl = impl_from_IFrameworkElement( element );
    IFrameworkElement_Release( element );
    return impl;
}

struct element *element_create( const WCHAR *type_name )
{
    struct element *impl;
    unsigned int i;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return NULL;
    impl->IFrameworkElement_iface.lpVtbl = &framework_vtbl;
    impl->IUIElement_iface.lpVtbl = &uielement_vtbl;
    impl->IDependencyObject_iface.lpVtbl = &depobj_vtbl;
    impl->ITextBlock_iface.lpVtbl = &textblock_vtbl;
    impl->IRangeBase_iface.lpVtbl = &rangebase_vtbl;
    impl->IProgressBar_iface.lpVtbl = &progressbar_vtbl;
    impl->ISwapChainPanel_iface.lpVtbl = &swapchainpanel_vtbl;
    impl->ISwapChainPanelNative_iface.lpVtbl = &swapchain_native_vtbl;
    impl->ref = 1;
    list_init( &impl->children );

    impl->kind = ELEMENT_UNKNOWN;
    for (i = 0; i < ARRAY_SIZE(element_types); i++)
        if (!wcscmp( type_name, element_types[i].name )) impl->kind = element_types[i].kind;
    if (!(impl->type_name = wcsdup( type_name )))
    {
        free( impl );
        return NULL;
    }

    impl->width = impl->height = NAN;
    impl->max_width = impl->max_height = INFINITY;
    impl->halign = HALIGN_STRETCH;
    impl->valign = VALIGN_STRETCH;
    impl->opacity = 1.0;
    impl->font_size = 14.0;
    impl->font_weight = 400;
    impl->text_alignment = TEXTALIGN_LEFT;
    impl->foreground.set = TRUE;
    impl->foreground.argb = 0xff000000;
    impl->maximum = 100.0;
    impl->stroke_thickness = 1.0;
    impl->geometry.nonzero = FALSE;

    switch (impl->kind)
    {
    case ELEMENT_PROGRESSBAR:
        impl->min_height = 4.0;
        impl->foreground.argb = 0xff0078d7;
        impl->background.set = TRUE;
        impl->background.argb = 0x33000000;
        break;
    case ELEMENT_VIEWBOX:
        impl->stretch = STRETCH_UNIFORM;
        break;
    case ELEMENT_RECTANGLE:
        impl->stretch = STRETCH_FILL;
        break;
    default:
        break;
    }

    return impl;
}

void element_add_child( struct element *parent, struct element *child )
{
    IFrameworkElement_AddRef( &child->IFrameworkElement_iface );
    child->parent = parent;
    list_add_tail( &parent->children, &child->entry );
}

struct element *element_find_name( struct element *root, const WCHAR *name )
{
    struct element *child, *found;

    if (root->name && !wcscmp( root->name, name )) return root;
    LIST_FOR_EACH_ENTRY( child, &root->children, struct element, entry )
        if ((found = element_find_name( child, name ))) return found;
    return NULL;
}

void element_set_host( struct element *element, struct xaml_source *host )
{
    element->host = host;
}

void element_invalidate( struct element *element )
{
    HWND hwnd = NULL;

    EnterCriticalSection( &xaml_cs );
    while (element->parent) element = element->parent;
    if (element->host) hwnd = element->host->hwnd;
    LeaveCriticalSection( &xaml_cs );

    if (hwnd) InvalidateRect( hwnd, NULL, FALSE );
}

BOOL element_needs_animation( struct element *element )
{
    struct element *child;

    if (element->visibility) return FALSE;
    if (element->kind == ELEMENT_PROGRESSBAR && element->indeterminate) return TRUE;
    LIST_FOR_EACH_ENTRY( child, &element->children, struct element, entry )
        if (element_needs_animation( child )) return TRUE;
    return FALSE;
}
