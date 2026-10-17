/*
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
#define COBJMACROS
#include "initguid.h"
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winstring.h"

#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_UI
#include "windows.ui.h"
#define WIDL_using_Windows_UI_Xaml
#define WIDL_using_Windows_UI_Xaml_Controls
#define WIDL_using_Windows_UI_Xaml_Controls_Primitives
#define WIDL_using_Windows_UI_Xaml_Hosting
#define WIDL_using_Windows_UI_Xaml_Markup
#include "windows.ui.xaml.h"
#include "xaml_abi.h"
#include "windows.ui.xaml.hosting.desktopwindowxamlsource.h"
#include "winuser.h"

#include "wine/test.h"

/* FiveM updater markup as formatted by the GTA_FIVE build of UpdaterUI.cpp */
static const WCHAR fivem_updater_xaml[] =
    L"\n"
    L"<Grid\n"
    L"    xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\"\n"
    L"    xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\"\n"
    L"    xmlns:d=\"http://schemas.microsoft.com/expression/blend/2008\"\n"
    L"    xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\"\n"
    L"\txmlns:local=\"using:CitiLaunch\"\n"
    L"    mc:Ignorable=\"d\">\n"
    L"\n"
    L"    <Grid Width=\"525\" Height=\"525\" Background=\"#202020\">\n"
    L"        <Grid x:Name=\"BackdropGrid\" />\n"
    L"\t\t<SwapChainPanel x:Name=\"Overlay\" />\n"
    L"\n"
    L"\t\t<!-- Icon -->\n"
    L"\t\t\n"
    L"<Viewbox Height=\"150\" Margin=\"0,0,0,60\" VerticalAlignment=\"Center\" HorizontalAlignment=\"Cente"
    L"r\" RenderTransformOrigin=\"0.5,0.5\">\n"
    L"\t<Path\n"
    L"\t\tData=\"M37.97 86.09L108.57 15.84L113.3 30.07L28.37 114.9L37.97 86.09ZM106.1 8.42L103.3 0H71.84C6"
    L"8.74 0 66 1.97 65.03 4.91L42.9 71.31L106.1 8.42ZM18.48 144.56L0 200H40.41L73.07 90.04L18.48 144.56ZM"
    L"118.25 44.92L91.49 71.65L129.41 200H169.85L118.25 44.92Z\" Stretch=\"Fill\"\n"
    L"\t\tFill=\"#66AD43\"\n"
    L"\t>\n"
    L"\t</Path>\n"
    L"</Viewbox>\n"
    L"\n"
    L"\n"
    L"\t\t<StackPanel Orientation=\"Vertical\" VerticalAlignment=\"Bottom\" Margin=\"40,0,40,40\">\n"
    L"            <TextBlock x:Name=\"static2\" Text=\" \" TextAlignment=\"Left\" Foreground=\"#ABABAB\" F"
    L"ontSize=\"12\" FontWeight=\"Bold\" />\n"
    L"\t\t\t<Grid Margin=\"0,8,0,8\">\n"
    L"\t\t\t\t<ProgressBar x:Name=\"progressBar\" Foreground=\"#66AD43\" Background=\"#090A09\" Horizontal"
    L"Alignment=\"Stretch\" MinHeight=\"6\" CornerRadius=\"3\" />\n"
    L"\t\t\t</Grid>\n"
    L"            <TextBlock x:Name=\"static1\" Text=\" \" TextAlignment=\"Left\" Foreground=\"#ABABAB\" F"
    L"ontSize=\"10\" FontWeight=\"Bold\" />\n"
    L"        </StackPanel>\n"
    L"    </Grid>\n"
    L"</Grid>\n"
    L"\n";

#define check_interface( obj, iid, is_broken ) check_interface_( __LINE__, obj, iid, is_broken )
static void check_interface_( unsigned int line, void *obj, const IID *iid, BOOL is_broken )
{
    IUnknown *iface = obj;
    IUnknown *unk;
    HRESULT hr;

    hr = IUnknown_QueryInterface( iface, iid, (void **)&unk );
    ok_(__FILE__, line)( hr == S_OK || broken( is_broken && hr == E_NOINTERFACE ), "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
        IUnknown_Release( unk );
}

static void test_ColorHelper(void)
{
    static const WCHAR *color_helper_statics_name = L"Windows.UI.ColorHelper";
    IColorHelperStatics *color_helper_statics = (void *)0xdeadbeef;
    IActivationFactory *factory = (void *)0xdeadbeef;
    Color color;
    HSTRING str;
    HRESULT hr;

    hr = WindowsCreateString( color_helper_statics_name, wcslen( color_helper_statics_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( color_helper_statics_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown, FALSE );
    check_interface( factory, &IID_IInspectable, FALSE );
    check_interface( factory, &IID_IAgileObject, TRUE /* Missing on Windows older than 1809v2 */ );

    hr = IActivationFactory_QueryInterface( factory, &IID_IColorHelperStatics, (void **)&color_helper_statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    memset( &color, 0, sizeof( color ) );
    hr = IColorHelperStatics_FromArgb( color_helper_statics, 255, 255, 255, 255, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IColorHelperStatics_FromArgb( color_helper_statics, 255, 255, 255, 255, &color );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( color.A == 255 && color.R == 255 && color.G == 255 && color.B == 255,
        "got color.A = %u, color.R = %u, color.G = %u, color.B = %u,\n", color.A, color.R, color.G, color.B );

    IColorHelperStatics_Release( color_helper_statics );
    IActivationFactory_Release( factory );
}

/* Shape of the official FiveM updater UI (code/client/launcher/UpdaterUI.cpp) */
static const WCHAR updater_xaml[] =
    L"<Grid xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\""
    L" xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\""
    L" xmlns:d=\"http://schemas.microsoft.com/expression/blend/2008\""
    L" xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\""
    L" xmlns:local=\"using:CitiLaunch\" mc:Ignorable=\"d\">"
    L"<Grid Width=\"525\" Height=\"525\" Background=\"#161923\">"
    L"<Grid x:Name=\"BackdropGrid\" /><SwapChainPanel x:Name=\"Overlay\" />"
    L"<Viewbox Height=\"150\" Margin=\"0,0,0,60\" VerticalAlignment=\"Center\" HorizontalAlignment=\"Center\">"
    L"<Path Data=\"M0 0L10 0L10 20ZM2 2H4V4H2Z\" Stretch=\"Fill\" Fill=\"#D80D0D\"></Path></Viewbox>"
    L"<StackPanel Orientation=\"Vertical\" VerticalAlignment=\"Bottom\" Margin=\"40,0,40,40\">"
    L"<TextBlock x:Name=\"static2\" Text=\" \" TextAlignment=\"Left\" Foreground=\"#ffffff\" FontSize=\"12\" FontWeight=\"Bold\" />"
    L"<Grid Margin=\"0,8,0,8\"><ProgressBar x:Name=\"progressBar\" Foreground=\"#D80D0D\" Background=\"#33ffffff\""
    L" HorizontalAlignment=\"Stretch\" MinHeight=\"6\" CornerRadius=\"3\" /></Grid>"
    L"<TextBlock x:Name=\"static1\" Text=\" \" TextAlignment=\"Left\" Foreground=\"#cccccc\" FontSize=\"10\" FontWeight=\"Bold\" />"
    L"</StackPanel></Grid></Grid>";

static HRESULT get_factory( const WCHAR *name, const IID *iid, void **out )
{
    IActivationFactory *factory;
    HSTRING str;
    HRESULT hr;

    WindowsCreateString( name, wcslen( name ), &str );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    if (FAILED(hr)) return hr;
    hr = IActivationFactory_QueryInterface( factory, iid, out );
    IActivationFactory_Release( factory );
    return hr;
}

static IInspectable *find_name( IFrameworkElement *root, const WCHAR *name )
{
    IInspectable *result = (void *)0xdeadbeef;
    HSTRING str;
    HRESULT hr;

    WindowsCreateString( name, wcslen( name ), &str );
    hr = IFrameworkElement_FindName( root, str, &result );
    WindowsDeleteString( str );
    ok( hr == S_OK, "FindName(%s) got hr %#lx.\n", wine_dbgstr_w( name ), hr );
    return result;
}

static HRESULT load_xaml( const WCHAR *xaml, IInspectable **result )
{
    IXamlReaderStatics *reader;
    HSTRING str;
    HRESULT hr;

    hr = get_factory( L"Windows.UI.Xaml.Markup.XamlReader", &IID_IXamlReaderStatics, (void **)&reader );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return hr;
    WindowsCreateString( xaml, wcslen( xaml ), &str );
    hr = IXamlReaderStatics_Load( reader, str, result );
    WindowsDeleteString( str );
    IXamlReaderStatics_Release( reader );
    return hr;
}

static void test_WindowsXamlManager(void)
{
    IWindowsXamlManagerStatics *statics;
    IInspectable *manager, *manager2;
    IClosable *closable;
    HRESULT hr;

    hr = get_factory( L"Windows.UI.Xaml.Hosting.WindowsXamlManager", &IID_IWindowsXamlManagerStatics, (void **)&statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return;

    hr = IWindowsXamlManagerStatics_InitializeForCurrentThread( statics, &manager );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_interface( manager, &IID_IWindowsXamlManager, FALSE );
    hr = IWindowsXamlManagerStatics_InitializeForCurrentThread( statics, &manager2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( manager == manager2, "got different managers %p, %p for the same thread.\n", manager, manager2 );
    IInspectable_Release( manager2 );

    hr = IInspectable_QueryInterface( manager, &IID_IClosable, (void **)&closable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IClosable_Close( closable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IClosable_Release( closable );
    IInspectable_Release( manager );
    IWindowsXamlManagerStatics_Release( statics );
}

static void test_XamlReader(void)
{
    IInspectable *doc, *obj, *content;
    IFrameworkElement *root;
    ITextBlock *text;
    IRangeBase *range;
    IProgressBar *progress;
    HSTRING str;
    boolean indeterminate;
    double value;
    HRESULT hr;

    hr = load_xaml( L"<Grid xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\"><Grid>", &doc );
    ok( FAILED(hr), "got hr %#lx.\n", hr );

    hr = load_xaml( fivem_updater_xaml, &doc );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr)) IInspectable_Release( doc );

    hr = load_xaml( updater_xaml, &doc );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return;

    hr = IInspectable_QueryInterface( doc, &IID_IFrameworkElement, (void **)&root );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_interface( doc, &IID_IUIElement, FALSE );
    check_interface( doc, &IID_IDependencyObject, FALSE );

    obj = find_name( root, L"missing" );
    ok( !obj, "got %p.\n", obj );

    obj = find_name( root, L"Overlay" );
    ok( !!obj, "Overlay not found.\n" );
    check_interface( obj, &IID_ISwapChainPanel, FALSE );
    check_interface( obj, &IID_ISwapChainPanelNative, FALSE );
    IInspectable_Release( obj );

    obj = find_name( root, L"static1" );
    ok( !!obj, "static1 not found.\n" );
    hr = IInspectable_QueryInterface( obj, &IID_ITextBlock, (void **)&text );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IInspectable_Release( obj );
    WindowsCreateString( L"Downloading", 11, &str );
    hr = ITextBlock_put_Text( text, str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    hr = ITextBlock_get_Text( text, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"Downloading" ), "got %s.\n", debugstr_hstring( str ) );
    WindowsDeleteString( str );
    ITextBlock_Release( text );

    obj = find_name( root, L"progressBar" );
    ok( !!obj, "progressBar not found.\n" );
    hr = IInspectable_QueryInterface( obj, &IID_IProgressBar, (void **)&progress );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IInspectable_QueryInterface( obj, &IID_IRangeBase, (void **)&range );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IInspectable_Release( obj );

    hr = IRangeBase_put_Maximum( range, 100.0 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IRangeBase_put_Value( range, 42.5 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IRangeBase_get_Value( range, &value );
    ok( hr == S_OK && value == 42.5, "got hr %#lx, value %f.\n", hr, value );
    hr = IRangeBase_put_Value( range, 150.0 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IRangeBase_get_Value( range, &value );
    ok( hr == S_OK && value == 100.0, "got hr %#lx, value %f.\n", hr, value );
    hr = IProgressBar_put_IsIndeterminate( progress, TRUE );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IProgressBar_get_IsIndeterminate( progress, &indeterminate );
    ok( hr == S_OK && indeterminate, "got hr %#lx, indeterminate %d.\n", hr, indeterminate );
    IProgressBar_Release( progress );
    IRangeBase_Release( range );

    hr = IFrameworkElement_get_Parent( root, &content );
    ok( hr == S_OK && !content, "got hr %#lx, parent %p.\n", hr, content );

    IFrameworkElement_Release( root );
    IInspectable_Release( doc );
}

static void test_DesktopWindowXamlSource(void)
{
    IDesktopWindowXamlSourceFactory *factory;
    IDesktopWindowXamlSourceNative *native;
    IWindowsXamlManagerStatics *statics;
    IInspectable *manager, *inner, *obj, *doc, *content;
    IDesktopWindowXamlSource *source;
    IClosable *closable;
    HWND parent, child;
    MSG msg;
    HRESULT hr;

    hr = get_factory( L"Windows.UI.Xaml.Hosting.WindowsXamlManager", &IID_IWindowsXamlManagerStatics, (void **)&statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return;
    hr = IWindowsXamlManagerStatics_InitializeForCurrentThread( statics, &manager );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IWindowsXamlManagerStatics_Release( statics );

    hr = get_factory( L"Windows.UI.Xaml.Hosting.DesktopWindowXamlSource", &IID_IDesktopWindowXamlSourceFactory, (void **)&factory );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) goto done;

    inner = (void *)0xdeadbeef;
    hr = IDesktopWindowXamlSourceFactory_CreateInstance( factory, NULL, &inner, &obj );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (inner) IInspectable_Release( inner );
    IDesktopWindowXamlSourceFactory_Release( factory );

    hr = IInspectable_QueryInterface( obj, &IID_IDesktopWindowXamlSource, (void **)&source );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IInspectable_QueryInterface( obj, &IID_IDesktopWindowXamlSourceNative, (void **)&native );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IInspectable_Release( obj );

    parent = CreateWindowW( L"static", L"host", WS_POPUP | WS_VISIBLE, 0, 0, 525, 525, NULL, NULL, NULL, NULL );
    ok( !!parent, "CreateWindowW failed.\n" );

    child = (HWND)0xdeadbeef;
    hr = IDesktopWindowXamlSourceNative_get_WindowHandle( native, &child );
    ok( hr == S_OK && !child, "got hr %#lx, hwnd %p.\n", hr, child );
    hr = IDesktopWindowXamlSourceNative_AttachToWindow( native, parent );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IDesktopWindowXamlSourceNative_get_WindowHandle( native, &child );
    ok( hr == S_OK && child, "got hr %#lx, hwnd %p.\n", hr, child );
    ok( IsChild( parent, child ), "%p is not a child of %p.\n", child, parent );

    hr = load_xaml( updater_xaml, &doc );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IInspectable_QueryInterface( doc, &IID_IUIElement, (void **)&content );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IDesktopWindowXamlSource_put_Content( source, content );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IInspectable_Release( content );
    IInspectable_Release( doc );

    hr = IDesktopWindowXamlSource_get_Content( source, &content );
    ok( hr == S_OK && content, "got hr %#lx, content %p.\n", hr, content );
    if (content) IInspectable_Release( content );

    SetWindowLongW( child, GWL_EXSTYLE, GetWindowLongW( child, GWL_EXSTYLE ) | WS_EX_TRANSPARENT | WS_EX_LAYERED );
    SetWindowPos( child, 0, 0, 0, 525, 525, SWP_SHOWWINDOW );
    UpdateWindow( child );
    while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE )) DispatchMessageW( &msg );

    hr = IDesktopWindowXamlSource_QueryInterface( source, &IID_IClosable, (void **)&closable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IClosable_Close( closable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IClosable_Release( closable );
    ok( !IsWindow( child ), "bridge window %p still exists after Close.\n", child );

    IDesktopWindowXamlSourceNative_Release( native );
    IDesktopWindowXamlSource_Release( source );
    DestroyWindow( parent );
done:
    IInspectable_Release( manager );
}

static DWORD WINAPI hosting_thread( void *arg )
{
    HRESULT hr;

    /* XAML hosting requires a single-threaded apartment */
    hr = RoInitialize( RO_INIT_SINGLETHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_WindowsXamlManager();
    test_XamlReader();
    test_DesktopWindowXamlSource();

    RoUninitialize();
    return 0;
}

START_TEST(xaml)
{
    HANDLE thread;
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_ColorHelper();

    RoUninitialize();

    thread = CreateThread( NULL, 0, hosting_thread, NULL, 0, NULL );
    WaitForSingleObject( thread, INFINITE );
    CloseHandle( thread );
}
