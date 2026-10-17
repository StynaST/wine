/* WinRT Windows.UI.Xaml.Hosting Implementation
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

#include "private.h"
#include "winuser.h"
#include "commctrl.h"

WINE_DEFAULT_DEBUG_CHANNEL(xaml);

#define ANIMATION_TIMER 1
#define ANIMATION_INTERVAL 16

static const WCHAR bridge_class[] = L"Windows.UI.Composition.DesktopWindowContentBridge";

static CRITICAL_SECTION_DEBUG xaml_cs_debug =
{
    0, 0, &xaml_cs,
    { &xaml_cs_debug.ProcessLocksList, &xaml_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": xaml_cs") }
};
CRITICAL_SECTION xaml_cs = { &xaml_cs_debug, -1, 0, 0, 0, 0 };

/* Windows.UI.Xaml.Hosting.WindowsXamlManager */

struct xaml_manager
{
    IWindowsXamlManager IWindowsXamlManager_iface;
    IClosable IClosable_iface;
    LONG ref;
    struct list entry;
    DWORD thread_id;
    BOOL closed;
};

static struct list managers = LIST_INIT( managers );

static inline struct xaml_manager *impl_from_IWindowsXamlManager( IWindowsXamlManager *iface )
{
    return CONTAINING_RECORD( iface, struct xaml_manager, IWindowsXamlManager_iface );
}

static HRESULT WINAPI manager_QueryInterface( IWindowsXamlManager *iface, REFIID iid, void **out )
{
    struct xaml_manager *impl = impl_from_IWindowsXamlManager( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IWindowsXamlManager ))
        *out = &impl->IWindowsXamlManager_iface;
    else if (IsEqualGUID( iid, &IID_IClosable ))
        *out = &impl->IClosable_iface;
    else
    {
        *out = NULL;
        WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI manager_AddRef( IWindowsXamlManager *iface )
{
    struct xaml_manager *impl = impl_from_IWindowsXamlManager( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI manager_Release( IWindowsXamlManager *iface )
{
    struct xaml_manager *impl = impl_from_IWindowsXamlManager( iface );
    ULONG ref;

    EnterCriticalSection( &xaml_cs );
    if (!(ref = InterlockedDecrement( &impl->ref ))) list_remove( &impl->entry );
    LeaveCriticalSection( &xaml_cs );

    TRACE( "iface %p, ref %lu.\n", iface, ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI manager_GetIids( IWindowsXamlManager *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI manager_GetRuntimeClassName( IWindowsXamlManager *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    return WindowsCreateString( RuntimeClass_Windows_UI_Xaml_Hosting_WindowsXamlManager,
                                wcslen( RuntimeClass_Windows_UI_Xaml_Hosting_WindowsXamlManager ), class_name );
}

static HRESULT WINAPI manager_GetTrustLevel( IWindowsXamlManager *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    *trust_level = BaseTrust;
    return S_OK;
}

static const struct IWindowsXamlManagerVtbl manager_vtbl =
{
    manager_QueryInterface,
    manager_AddRef,
    manager_Release,
    /* IInspectable methods */
    manager_GetIids,
    manager_GetRuntimeClassName,
    manager_GetTrustLevel,
};

DEFINE_IINSPECTABLE_( manager_closable, IClosable, struct xaml_manager, manager_from_IClosable, IClosable_iface,
                      &impl->IWindowsXamlManager_iface )

static HRESULT WINAPI manager_closable_Close( IClosable *iface )
{
    struct xaml_manager *impl = manager_from_IClosable( iface );

    TRACE( "iface %p.\n", iface );

    EnterCriticalSection( &xaml_cs );
    impl->closed = TRUE;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static const struct IClosableVtbl manager_closable_vtbl =
{
    manager_closable_QueryInterface,
    manager_closable_AddRef,
    manager_closable_Release,
    /* IInspectable methods */
    manager_closable_GetIids,
    manager_closable_GetRuntimeClassName,
    manager_closable_GetTrustLevel,
    /* IClosable methods */
    manager_closable_Close,
};

static HRESULT get_thread_manager( IWindowsXamlManager **result )
{
    DWORD tid = GetCurrentThreadId();
    struct xaml_manager *impl;

    EnterCriticalSection( &xaml_cs );
    LIST_FOR_EACH_ENTRY( impl, &managers, struct xaml_manager, entry )
    {
        if (impl->thread_id == tid && !impl->closed)
        {
            /* the list entry holds no reference, so a dying manager can still be listed */
            if (InterlockedIncrement( &impl->ref ) > 1)
            {
                LeaveCriticalSection( &xaml_cs );
                *result = &impl->IWindowsXamlManager_iface;
                return S_OK;
            }
            InterlockedDecrement( &impl->ref );
        }
    }

    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        LeaveCriticalSection( &xaml_cs );
        return E_OUTOFMEMORY;
    }
    impl->IWindowsXamlManager_iface.lpVtbl = &manager_vtbl;
    impl->IClosable_iface.lpVtbl = &manager_closable_vtbl;
    impl->ref = 1;
    impl->thread_id = tid;
    list_add_tail( &managers, &impl->entry );
    LeaveCriticalSection( &xaml_cs );

    *result = &impl->IWindowsXamlManager_iface;
    return S_OK;
}

/* Windows.UI.Xaml.Hosting.DesktopWindowXamlSource */

static inline struct xaml_source *impl_from_IDesktopWindowXamlSource( IDesktopWindowXamlSource *iface )
{
    return CONTAINING_RECORD( iface, struct xaml_source, IDesktopWindowXamlSource_iface );
}

static void source_detach_content( struct xaml_source *impl )
{
    struct element *content;

    EnterCriticalSection( &xaml_cs );
    if ((content = impl->content)) element_set_host( content, NULL );
    impl->content = NULL;
    LeaveCriticalSection( &xaml_cs );
    if (content) IFrameworkElement_Release( &content->IFrameworkElement_iface );
}

/* XAML content is composited above the host window. Keep host GDI painting
 * from leaving its pixels over the bridge window when the host does not clip
 * its children. */
static LRESULT CALLBACK host_subclass_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                            UINT_PTR id, DWORD_PTR data )
{
    struct xaml_source *impl = (struct xaml_source *)data;
    HWND bridge = impl->hwnd;
    RECT rect, update;
    LRESULT ret;

    if (msg == WM_NCDESTROY)
    {
        RemoveWindowSubclass( hwnd, host_subclass_proc, id );
        return DefSubclassProc( hwnd, msg, wparam, lparam );
    }
    if ((msg != WM_ERASEBKGND && msg != WM_PAINT) || !bridge || !IsWindowVisible( bridge ) ||
        (GetWindowLongW( hwnd, GWL_STYLE ) & WS_CLIPCHILDREN))
        return DefSubclassProc( hwnd, msg, wparam, lparam );

    GetWindowRect( bridge, &rect );
    MapWindowPoints( NULL, hwnd, (POINT *)&rect, 2 );
    if (msg == WM_ERASEBKGND)
    {
        ExcludeClipRect( (HDC)wparam, rect.left, rect.top, rect.right, rect.bottom );
        return DefSubclassProc( hwnd, msg, wparam, lparam );
    }

    GetUpdateRect( hwnd, &update, FALSE );
    ret = DefSubclassProc( hwnd, msg, wparam, lparam );
    if (IntersectRect( &update, &update, &rect ))
    {
        OffsetRect( &update, -rect.left, -rect.top );
        RedrawWindow( bridge, &update, NULL, RDW_INVALIDATE | RDW_NOERASE );
    }
    return ret;
}

static void source_close( struct xaml_source *impl )
{
    HWND hwnd, parent;

    EnterCriticalSection( &xaml_cs );
    hwnd = impl->hwnd;
    impl->hwnd = NULL;
    LeaveCriticalSection( &xaml_cs );

    if (hwnd)
    {
        if ((parent = GetParent( hwnd ))) RemoveWindowSubclass( parent, host_subclass_proc, (UINT_PTR)impl );
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, 0 );
        DestroyWindow( hwnd );
    }
    source_detach_content( impl );
}

static HRESULT WINAPI source_QueryInterface( IDesktopWindowXamlSource *iface, REFIID iid, void **out )
{
    struct xaml_source *impl = impl_from_IDesktopWindowXamlSource( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IDesktopWindowXamlSource ))
        *out = &impl->IDesktopWindowXamlSource_iface;
    else if (IsEqualGUID( iid, &IID_IClosable ))
        *out = &impl->IClosable_iface;
    else if (IsEqualGUID( iid, &IID_IDesktopWindowXamlSourceNative ) ||
             IsEqualGUID( iid, &IID_IDesktopWindowXamlSourceNative2 ))
        *out = &impl->IDesktopWindowXamlSourceNative2_iface;
    else
    {
        *out = NULL;
        WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI source_AddRef( IDesktopWindowXamlSource *iface )
{
    struct xaml_source *impl = impl_from_IDesktopWindowXamlSource( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI source_Release( IDesktopWindowXamlSource *iface )
{
    struct xaml_source *impl = impl_from_IDesktopWindowXamlSource( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        source_close( impl );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI source_GetIids( IDesktopWindowXamlSource *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI source_GetRuntimeClassName( IDesktopWindowXamlSource *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    return WindowsCreateString( RuntimeClass_Windows_UI_Xaml_Hosting_DesktopWindowXamlSource,
                                wcslen( RuntimeClass_Windows_UI_Xaml_Hosting_DesktopWindowXamlSource ), class_name );
}

static HRESULT WINAPI source_GetTrustLevel( IDesktopWindowXamlSource *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI source_get_Content( IDesktopWindowXamlSource *iface, IInspectable **value )
{
    struct xaml_source *impl = impl_from_IDesktopWindowXamlSource( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    EnterCriticalSection( &xaml_cs );
    if (impl->content)
        IFrameworkElement_QueryInterface( &impl->content->IFrameworkElement_iface, &IID_IUIElement, (void **)value );
    else
        *value = NULL;
    LeaveCriticalSection( &xaml_cs );
    return S_OK;
}

static HRESULT WINAPI source_put_Content( IDesktopWindowXamlSource *iface, IInspectable *value )
{
    struct xaml_source *impl = impl_from_IDesktopWindowXamlSource( iface );
    struct element *element = NULL;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (value && !(element = unsafe_impl_from_IInspectable( value )))
    {
        FIXME( "content %p is not an element created by this module.\n", value );
        return E_INVALIDARG;
    }

    source_detach_content( impl );
    if (element)
    {
        IFrameworkElement_AddRef( &element->IFrameworkElement_iface );
        EnterCriticalSection( &xaml_cs );
        if (element->parent || element->host)
        {
            LeaveCriticalSection( &xaml_cs );
            IFrameworkElement_Release( &element->IFrameworkElement_iface );
            WARN( "element %p already has a parent.\n", element );
            return E_INVALIDARG;
        }
        impl->content = element;
        element_set_host( element, impl );
        LeaveCriticalSection( &xaml_cs );
    }
    if (impl->hwnd) InvalidateRect( impl->hwnd, NULL, FALSE );
    return S_OK;
}

static HRESULT WINAPI source_get_HasFocus( IDesktopWindowXamlSource *iface, boolean *value )
{
    struct xaml_source *impl = impl_from_IDesktopWindowXamlSource( iface );
    HWND focus = GetFocus();

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->hwnd && (focus == impl->hwnd || IsChild( impl->hwnd, focus ));
    return S_OK;
}

static HRESULT WINAPI source_add_TakeFocusRequested( IDesktopWindowXamlSource *iface, IInspectable *handler,
                                                     EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p semi-stub!\n", iface, handler, token );
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI source_remove_TakeFocusRequested( IDesktopWindowXamlSource *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %s semi-stub!\n", iface, wine_dbgstr_longlong( token.value ) );
    return S_OK;
}

static HRESULT WINAPI source_add_GotFocus( IDesktopWindowXamlSource *iface, IInspectable *handler,
                                           EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p semi-stub!\n", iface, handler, token );
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI source_remove_GotFocus( IDesktopWindowXamlSource *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %s semi-stub!\n", iface, wine_dbgstr_longlong( token.value ) );
    return S_OK;
}

static HRESULT WINAPI source_NavigateFocus( IDesktopWindowXamlSource *iface, IInspectable *request, IInspectable **result )
{
    FIXME( "iface %p, request %p, result %p stub!\n", iface, request, result );
    return E_NOTIMPL;
}

static const struct IDesktopWindowXamlSourceVtbl source_vtbl =
{
    source_QueryInterface,
    source_AddRef,
    source_Release,
    /* IInspectable methods */
    source_GetIids,
    source_GetRuntimeClassName,
    source_GetTrustLevel,
    /* IDesktopWindowXamlSource methods */
    source_get_Content,
    source_put_Content,
    source_get_HasFocus,
    source_add_TakeFocusRequested,
    source_remove_TakeFocusRequested,
    source_add_GotFocus,
    source_remove_GotFocus,
    source_NavigateFocus,
};

DEFINE_IINSPECTABLE_( source_closable, IClosable, struct xaml_source, source_from_IClosable, IClosable_iface,
                      &impl->IDesktopWindowXamlSource_iface )

static HRESULT WINAPI source_closable_Close( IClosable *iface )
{
    struct xaml_source *impl = source_from_IClosable( iface );

    TRACE( "iface %p.\n", iface );

    source_close( impl );
    return S_OK;
}

static const struct IClosableVtbl source_closable_vtbl =
{
    source_closable_QueryInterface,
    source_closable_AddRef,
    source_closable_Release,
    /* IInspectable methods */
    source_closable_GetIids,
    source_closable_GetRuntimeClassName,
    source_closable_GetTrustLevel,
    /* IClosable methods */
    source_closable_Close,
};

static inline struct xaml_source *impl_from_IDesktopWindowXamlSourceNative2( IDesktopWindowXamlSourceNative2 *iface )
{
    return CONTAINING_RECORD( iface, struct xaml_source, IDesktopWindowXamlSourceNative2_iface );
}

static HRESULT WINAPI source_native_QueryInterface( IDesktopWindowXamlSourceNative2 *iface, REFIID iid, void **out )
{
    struct xaml_source *impl = impl_from_IDesktopWindowXamlSourceNative2( iface );
    return IDesktopWindowXamlSource_QueryInterface( &impl->IDesktopWindowXamlSource_iface, iid, out );
}

static ULONG WINAPI source_native_AddRef( IDesktopWindowXamlSourceNative2 *iface )
{
    struct xaml_source *impl = impl_from_IDesktopWindowXamlSourceNative2( iface );
    return IDesktopWindowXamlSource_AddRef( &impl->IDesktopWindowXamlSource_iface );
}

static ULONG WINAPI source_native_Release( IDesktopWindowXamlSourceNative2 *iface )
{
    struct xaml_source *impl = impl_from_IDesktopWindowXamlSourceNative2( iface );
    return IDesktopWindowXamlSource_Release( &impl->IDesktopWindowXamlSource_iface );
}

static void paint_source( struct xaml_source *impl, HWND hwnd )
{
    PAINTSTRUCT ps;
    RECT rect;
    HDC hdc;

    hdc = BeginPaint( hwnd, &ps );
    GetClientRect( hwnd, &rect );
    render_content( impl->content, hdc, rect.right, rect.bottom, GetDpiForWindow( hwnd ) / 96.0,
                    GetTickCount() - impl->start_ticks );
    EndPaint( hwnd, &ps );

    EnterCriticalSection( &xaml_cs );
    if (impl->content && element_needs_animation( impl->content ))
    {
        if (!impl->animating) SetTimer( hwnd, ANIMATION_TIMER, ANIMATION_INTERVAL, NULL );
        impl->animating = TRUE;
    }
    else if (impl->animating)
    {
        KillTimer( hwnd, ANIMATION_TIMER );
        impl->animating = FALSE;
    }
    LeaveCriticalSection( &xaml_cs );
}

static LRESULT CALLBACK bridge_wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct xaml_source *impl = (struct xaml_source *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    TRACE( "hwnd %p, msg %#x, wparam %#Ix, lparam %#Ix.\n", hwnd, msg, wparam, lparam );

    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lparam)->lpCreateParams );
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (!impl) break;
        paint_source( impl, hwnd );
        return 0;
    case WM_TIMER:
        if (wparam == ANIMATION_TIMER) InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_SIZE:
    case WM_DPICHANGED_AFTERPARENT:
        InvalidateRect( hwnd, NULL, FALSE );
        break;
    case WM_STYLECHANGING:
        /* the bridge presents through composition, so a layered style set by the
         * host must not move it to a separate, never-updated layered surface */
        if (wparam == GWL_EXSTYLE) ((STYLESTRUCT *)lparam)->styleNew &= ~WS_EX_LAYERED;
        break;
    case WM_NCHITTEST:
        if (GetWindowLongW( hwnd, GWL_EXSTYLE ) & WS_EX_TRANSPARENT) return HTTRANSPARENT;
        break;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static BOOL WINAPI register_bridge_class( INIT_ONCE *once, void *param, void **context )
{
    WNDCLASSEXW wc = {sizeof(wc)};

    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = bridge_wndproc;
    wc.hInstance = GetModuleHandleW( L"windows.ui.xaml.dll" );
    wc.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    wc.lpszClassName = bridge_class;
    return RegisterClassExW( &wc ) != 0;
}

static HRESULT WINAPI source_native_AttachToWindow( IDesktopWindowXamlSourceNative2 *iface, HWND parent )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    struct xaml_source *impl = impl_from_IDesktopWindowXamlSourceNative2( iface );
    RECT rect;
    HWND hwnd;

    TRACE( "iface %p, parent %p.\n", iface, parent );

    if (!IsWindow( parent )) return E_INVALIDARG;
    if (impl->hwnd) return E_UNEXPECTED;
    if (!InitOnceExecuteOnce( &once, register_bridge_class, NULL, NULL ))
        return HRESULT_FROM_WIN32( GetLastError() );

    GetClientRect( parent, &rect );
    hwnd = CreateWindowExW( 0, bridge_class, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                            0, 0, rect.right, rect.bottom, parent, NULL,
                            GetModuleHandleW( L"windows.ui.xaml.dll" ), impl );
    if (!hwnd) return HRESULT_FROM_WIN32( GetLastError() );

    EnterCriticalSection( &xaml_cs );
    impl->hwnd = hwnd;
    LeaveCriticalSection( &xaml_cs );
    SetWindowSubclass( parent, host_subclass_proc, (UINT_PTR)impl, (DWORD_PTR)impl );
    return S_OK;
}

static HRESULT WINAPI source_native_get_WindowHandle( IDesktopWindowXamlSourceNative2 *iface, HWND *hwnd )
{
    struct xaml_source *impl = impl_from_IDesktopWindowXamlSourceNative2( iface );

    TRACE( "iface %p, hwnd %p.\n", iface, hwnd );

    if (!hwnd) return E_POINTER;
    *hwnd = impl->hwnd;
    return S_OK;
}

static HRESULT WINAPI source_native_PreTranslateMessage( IDesktopWindowXamlSourceNative2 *iface, const MSG *msg, BOOL *result )
{
    TRACE( "iface %p, msg %p, result %p.\n", iface, msg, result );

    if (!result) return E_POINTER;
    *result = FALSE;
    return S_OK;
}

static const struct IDesktopWindowXamlSourceNative2Vtbl source_native_vtbl =
{
    source_native_QueryInterface,
    source_native_AddRef,
    source_native_Release,
    /* IDesktopWindowXamlSourceNative methods */
    source_native_AttachToWindow,
    source_native_get_WindowHandle,
    /* IDesktopWindowXamlSourceNative2 methods */
    source_native_PreTranslateMessage,
};

static HRESULT create_source( IDesktopWindowXamlSource **out )
{
    struct xaml_source *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IDesktopWindowXamlSource_iface.lpVtbl = &source_vtbl;
    impl->IClosable_iface.lpVtbl = &source_closable_vtbl;
    impl->IDesktopWindowXamlSourceNative2_iface.lpVtbl = &source_native_vtbl;
    impl->ref = 1;
    impl->start_ticks = GetTickCount();

    *out = &impl->IDesktopWindowXamlSource_iface;
    return S_OK;
}

/* activation factories */

struct hosting_factory
{
    IActivationFactory IActivationFactory_iface;
    IWindowsXamlManagerStatics IWindowsXamlManagerStatics_iface;
    IDesktopWindowXamlSourceFactory IDesktopWindowXamlSourceFactory_iface;
    const WCHAR *class_name;
    LONG ref;
};

static inline struct hosting_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct hosting_factory, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct hosting_factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (impl->IWindowsXamlManagerStatics_iface.lpVtbl && IsEqualGUID( iid, &IID_IWindowsXamlManagerStatics ))
        *out = &impl->IWindowsXamlManagerStatics_iface;
    else if (impl->IDesktopWindowXamlSourceFactory_iface.lpVtbl && IsEqualGUID( iid, &IID_IDesktopWindowXamlSourceFactory ))
        *out = &impl->IDesktopWindowXamlSourceFactory_iface;
    else
    {
        *out = NULL;
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct hosting_factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct hosting_factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    struct hosting_factory *impl = impl_from_IActivationFactory( iface );
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    return WindowsCreateString( impl->class_name, wcslen( impl->class_name ), class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct hosting_factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (impl->IDesktopWindowXamlSourceFactory_iface.lpVtbl)
        return create_source( (IDesktopWindowXamlSource **)instance );
    *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( manager_statics, IWindowsXamlManagerStatics, struct hosting_factory, IActivationFactory_iface )

static HRESULT WINAPI manager_statics_InitializeForCurrentThread( IWindowsXamlManagerStatics *iface, IInspectable **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );

    if (!result) return E_POINTER;
    return get_thread_manager( (IWindowsXamlManager **)result );
}

static const struct IWindowsXamlManagerStaticsVtbl manager_statics_vtbl =
{
    manager_statics_QueryInterface,
    manager_statics_AddRef,
    manager_statics_Release,
    /* IInspectable methods */
    manager_statics_GetIids,
    manager_statics_GetRuntimeClassName,
    manager_statics_GetTrustLevel,
    /* IWindowsXamlManagerStatics methods */
    manager_statics_InitializeForCurrentThread,
};

DEFINE_IINSPECTABLE( source_factory, IDesktopWindowXamlSourceFactory, struct hosting_factory, IActivationFactory_iface )

static HRESULT WINAPI source_factory_CreateInstance( IDesktopWindowXamlSourceFactory *iface, IInspectable *outer,
                                                     IInspectable **inner, IInspectable **value )
{
    TRACE( "iface %p, outer %p, inner %p, value %p.\n", iface, outer, inner, value );

    if (!value) return E_POINTER;
    if (inner) *inner = NULL;
    *value = NULL;
    if (outer)
    {
        FIXME( "aggregation not supported.\n" );
        return CLASS_E_NOAGGREGATION;
    }
    return create_source( (IDesktopWindowXamlSource **)value );
}

static const struct IDesktopWindowXamlSourceFactoryVtbl source_factory_vtbl =
{
    source_factory_QueryInterface,
    source_factory_AddRef,
    source_factory_Release,
    /* IInspectable methods */
    source_factory_GetIids,
    source_factory_GetRuntimeClassName,
    source_factory_GetTrustLevel,
    /* IDesktopWindowXamlSourceFactory methods */
    source_factory_CreateInstance,
};

static struct hosting_factory manager_factory =
{
    {&factory_vtbl},
    {&manager_statics_vtbl},
    {NULL},
    RuntimeClass_Windows_UI_Xaml_Hosting_WindowsXamlManager,
    1,
};

static struct hosting_factory source_factory =
{
    {&factory_vtbl},
    {NULL},
    {&source_factory_vtbl},
    RuntimeClass_Windows_UI_Xaml_Hosting_DesktopWindowXamlSource,
    1,
};

IActivationFactory *xaml_manager_factory = &manager_factory.IActivationFactory_iface;
IActivationFactory *xaml_source_factory = &source_factory.IActivationFactory_iface;
