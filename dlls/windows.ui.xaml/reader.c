/* WinRT Windows.UI.Xaml.Markup.XamlReader Implementation
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
#include "shlwapi.h"
#include "initguid.h"
#include "xmllite.h"

WINE_DEFAULT_DEBUG_CHANNEL(xaml);

#define E_XAMLPARSEFAILED 0x802b000a

static const WCHAR presentation_ns[] = L"http://schemas.microsoft.com/winfx/2006/xaml/presentation";
static const WCHAR xaml_ns[] = L"http://schemas.microsoft.com/winfx/2006/xaml";

static BOOL parse_font_weight( const WCHAR *value, UINT16 *weight )
{
    static const struct { const WCHAR *name; UINT16 weight; } weights[] =
    {
        {L"Thin", 100}, {L"ExtraLight", 200}, {L"Light", 300}, {L"SemiLight", 350}, {L"Normal", 400},
        {L"Medium", 500}, {L"SemiBold", 600}, {L"Bold", 700}, {L"ExtraBold", 800}, {L"Black", 900},
        {L"ExtraBlack", 950},
    };
    unsigned int i;
    double num;

    for (i = 0; i < ARRAY_SIZE(weights); i++)
    {
        if (!wcsicmp( value, weights[i].name ))
        {
            *weight = weights[i].weight;
            return TRUE;
        }
    }
    if (!parse_double( value, &num ) || num < 1 || num > 999) return FALSE;
    *weight = num;
    return TRUE;
}

static BOOL set_brush( struct brush *brush, const WCHAR *value )
{
    if (!parse_color( value, &brush->argb )) return FALSE;
    brush->set = TRUE;
    return TRUE;
}

static BOOL set_attribute( struct element *element, const WCHAR *name, const WCHAR *value )
{
    double num;

    if (!wcscmp( name, L"Name" ))
    {
        free( element->name );
        return !!(element->name = wcsdup( value ));
    }
    if (!wcscmp( name, L"Width" )) return parse_double( value, &element->width );
    if (!wcscmp( name, L"Height" )) return parse_double( value, &element->height );
    if (!wcscmp( name, L"MinWidth" )) return parse_double( value, &element->min_width );
    if (!wcscmp( name, L"MinHeight" )) return parse_double( value, &element->min_height );
    if (!wcscmp( name, L"MaxWidth" )) return parse_double( value, &element->max_width );
    if (!wcscmp( name, L"MaxHeight" )) return parse_double( value, &element->max_height );
    if (!wcscmp( name, L"Margin" )) return parse_thickness( value, &element->margin );
    if (!wcscmp( name, L"Padding" )) return parse_thickness( value, &element->padding );
    if (!wcscmp( name, L"HorizontalAlignment" ))
    {
        if (!wcsicmp( value, L"Left" )) element->halign = HALIGN_LEFT;
        else if (!wcsicmp( value, L"Center" )) element->halign = HALIGN_CENTER;
        else if (!wcsicmp( value, L"Right" )) element->halign = HALIGN_RIGHT;
        else if (!wcsicmp( value, L"Stretch" )) element->halign = HALIGN_STRETCH;
        else return FALSE;
        return TRUE;
    }
    if (!wcscmp( name, L"VerticalAlignment" ))
    {
        if (!wcsicmp( value, L"Top" )) element->valign = VALIGN_TOP;
        else if (!wcsicmp( value, L"Center" )) element->valign = VALIGN_CENTER;
        else if (!wcsicmp( value, L"Bottom" )) element->valign = VALIGN_BOTTOM;
        else if (!wcsicmp( value, L"Stretch" )) element->valign = VALIGN_STRETCH;
        else return FALSE;
        return TRUE;
    }
    if (!wcscmp( name, L"Visibility" ))
    {
        if (!wcsicmp( value, L"Visible" )) element->visibility = 0;
        else if (!wcsicmp( value, L"Collapsed" )) element->visibility = 1;
        else return FALSE;
        return TRUE;
    }
    if (!wcscmp( name, L"Opacity" )) return parse_double( value, &element->opacity );
    if (!wcscmp( name, L"Background" )) return set_brush( &element->background, value );
    if (!wcscmp( name, L"Foreground" )) return set_brush( &element->foreground, value );
    if (!wcscmp( name, L"Fill" )) return set_brush( &element->fill, value );
    if (!wcscmp( name, L"Stroke" )) return set_brush( &element->stroke, value );
    if (!wcscmp( name, L"StrokeThickness" )) return parse_double( value, &element->stroke_thickness );
    if (!wcscmp( name, L"CornerRadius" )) return parse_double( value, &element->corner_radius );
    if (!wcscmp( name, L"Text" ))
    {
        free( element->text );
        return !!(element->text = wcsdup( value ));
    }
    if (!wcscmp( name, L"FontSize" )) return parse_double( value, &element->font_size );
    if (!wcscmp( name, L"FontWeight" )) return parse_font_weight( value, &element->font_weight );
    if (!wcscmp( name, L"TextAlignment" ))
    {
        if (!wcsicmp( value, L"Center" )) element->text_alignment = TEXTALIGN_CENTER;
        else if (!wcsicmp( value, L"Left" ) || !wcsicmp( value, L"Start" )) element->text_alignment = TEXTALIGN_LEFT;
        else if (!wcsicmp( value, L"Right" ) || !wcsicmp( value, L"End" )) element->text_alignment = TEXTALIGN_RIGHT;
        else if (!wcsicmp( value, L"Justify" )) element->text_alignment = TEXTALIGN_JUSTIFY;
        else return FALSE;
        return TRUE;
    }
    if (!wcscmp( name, L"Orientation" ))
    {
        if (!wcsicmp( value, L"Vertical" )) element->orientation = ORIENTATION_VERTICAL;
        else if (!wcsicmp( value, L"Horizontal" )) element->orientation = ORIENTATION_HORIZONTAL;
        else return FALSE;
        return TRUE;
    }
    if (!wcscmp( name, L"Stretch" ))
    {
        if (!wcsicmp( value, L"None" )) element->stretch = STRETCH_NONE;
        else if (!wcsicmp( value, L"Fill" )) element->stretch = STRETCH_FILL;
        else if (!wcsicmp( value, L"Uniform" )) element->stretch = STRETCH_UNIFORM;
        else if (!wcsicmp( value, L"UniformToFill" )) element->stretch = STRETCH_UNIFORM_TO_FILL;
        else return FALSE;
        return TRUE;
    }
    if (!wcscmp( name, L"Data" )) return parse_path_data( value, &element->geometry );
    if (!wcscmp( name, L"Minimum" )) return parse_double( value, &element->minimum );
    if (!wcscmp( name, L"Maximum" )) return parse_double( value, &element->maximum );
    if (!wcscmp( name, L"Value" ))
    {
        if (!parse_double( value, &num )) return FALSE;
        element->value = num;
        return TRUE;
    }
    if (!wcscmp( name, L"IsIndeterminate" ))
    {
        element->indeterminate = !wcsicmp( value, L"True" );
        return element->indeterminate || !wcsicmp( value, L"False" );
    }

    FIXME( "unsupported attribute %s=%s on %s, ignoring.\n", debugstr_w(name), debugstr_w(value),
           debugstr_w(element->type_name) );
    return TRUE;
}

static HRESULT read_attributes( IXmlReader *reader, struct element *element )
{
    const WCHAR *name, *prefix, *uri, *value;
    HRESULT hr;

    for (hr = IXmlReader_MoveToFirstAttribute( reader ); hr == S_OK; hr = IXmlReader_MoveToNextAttribute( reader ))
    {
        IXmlReader_GetLocalName( reader, &name, NULL );
        IXmlReader_GetPrefix( reader, &prefix, NULL );
        IXmlReader_GetNamespaceUri( reader, &uri, NULL );
        IXmlReader_GetValue( reader, &value, NULL );

        if (!wcscmp( prefix, L"xmlns" ) || (!*prefix && !wcscmp( name, L"xmlns" ))) continue;
        if (*uri && wcscmp( uri, presentation_ns ) && wcscmp( uri, xaml_ns )) continue; /* d:, mc:, etc. */
        if (!wcscmp( uri, xaml_ns ) && wcscmp( name, L"Name" ))
        {
            FIXME( "unsupported x:%s directive, ignoring.\n", debugstr_w(name) );
            continue;
        }
        if (!set_attribute( element, name, value ))
        {
            WARN( "invalid value %s for %s.\n", debugstr_w(value), debugstr_w(name) );
            return E_XAMLPARSEFAILED;
        }
    }
    return IXmlReader_MoveToElement( reader );
}

static HRESULT skip_subtree( IXmlReader *reader, UINT depth )
{
    XmlNodeType type;
    UINT cur;
    HRESULT hr;

    while ((hr = IXmlReader_Read( reader, &type )) == S_OK)
    {
        IXmlReader_GetDepth( reader, &cur );
        if (type == XmlNodeType_EndElement && cur == depth) return S_OK;
    }
    return FAILED(hr) ? hr : E_XAMLPARSEFAILED;
}

HRESULT xaml_parse( const WCHAR *xaml, struct element **root )
{
    struct element *stack[64], *element;
    IXmlReader *reader = NULL;
    IStream *stream = NULL;
    UINT depth = 0, len;
    XmlNodeType type;
    const WCHAR *name, *value;
    HRESULT hr;
    char *utf8;

    *root = NULL;

    len = WideCharToMultiByte( CP_UTF8, 0, xaml, -1, NULL, 0, NULL, NULL );
    if (!(utf8 = malloc( len ))) return E_OUTOFMEMORY;
    WideCharToMultiByte( CP_UTF8, 0, xaml, -1, utf8, len, NULL, NULL );
    stream = SHCreateMemStream( (BYTE *)utf8, len - 1 );
    free( utf8 );
    if (!stream) return E_OUTOFMEMORY;

    if (FAILED(hr = CreateXmlReader( &IID_IXmlReader, (void **)&reader, NULL ))) goto done;
    IXmlReader_SetProperty( reader, XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit );
    if (FAILED(hr = IXmlReader_SetInput( reader, (IUnknown *)stream ))) goto done;

    while ((hr = IXmlReader_Read( reader, &type )) == S_OK)
    {
        TRACE( "node type %d, depth %u.\n", type, depth );
        switch (type)
        {
        case XmlNodeType_Element:
        {
            BOOL empty = IXmlReader_IsEmptyElement( reader );
            UINT node_depth;

            IXmlReader_GetLocalName( reader, &name, NULL );
            IXmlReader_GetDepth( reader, &node_depth );

            if (wcschr( name, '.' ))
            {
                FIXME( "property element %s not supported, skipping.\n", debugstr_w(name) );
                if (!empty && FAILED(hr = skip_subtree( reader, node_depth ))) goto done;
                break;
            }
            if (depth == ARRAY_SIZE(stack))
            {
                hr = E_XAMLPARSEFAILED;
                goto done;
            }
            if (!(element = element_create( name )))
            {
                hr = E_OUTOFMEMORY;
                goto done;
            }
            if (element->kind == ELEMENT_UNKNOWN)
                FIXME( "unsupported element %s, it will not be rendered.\n", debugstr_w(name) );

            if (FAILED(hr = read_attributes( reader, element )))
            {
                IFrameworkElement_Release( &element->IFrameworkElement_iface );
                goto done;
            }

            if (depth)
            {
                element_add_child( stack[depth - 1], element );
                IFrameworkElement_Release( &element->IFrameworkElement_iface );
            }
            else if (*root)
            {
                IFrameworkElement_Release( &element->IFrameworkElement_iface );
                hr = E_XAMLPARSEFAILED;
                goto done;
            }
            else *root = element;

            if (!empty) stack[depth++] = element;
            break;
        }
        case XmlNodeType_EndElement:
            IXmlReader_GetLocalName( reader, &name, NULL );
            if (!wcschr( name, '.' ) && depth) depth--;
            break;
        case XmlNodeType_Text:
        case XmlNodeType_CDATA:
            if (depth && stack[depth - 1]->kind == ELEMENT_TEXTBLOCK)
            {
                IXmlReader_GetValue( reader, &value, NULL );
                free( stack[depth - 1]->text );
                stack[depth - 1]->text = wcsdup( value );
            }
            break;
        default:
            break;
        }
    }
    TRACE( "read loop ended, hr %#lx, root %p.\n", hr, *root );
    if (hr == S_FALSE) hr = *root ? S_OK : E_XAMLPARSEFAILED;

done:
    if (FAILED(hr) && *root)
    {
        IFrameworkElement_Release( &(*root)->IFrameworkElement_iface );
        *root = NULL;
    }
    if (reader) IXmlReader_Release( reader );
    IStream_Release( stream );
    return hr;
}

struct reader_statics
{
    IActivationFactory IActivationFactory_iface;
    IXamlReaderStatics IXamlReaderStatics_iface;
    LONG ref;
};

static inline struct reader_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct reader_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct reader_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IXamlReaderStatics ))
    {
        *out = &impl->IXamlReaderStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct reader_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct reader_statics *impl = impl_from_IActivationFactory( iface );
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
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
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

DEFINE_IINSPECTABLE( reader_statics, IXamlReaderStatics, struct reader_statics, IActivationFactory_iface )

static HRESULT WINAPI reader_statics_Load( IXamlReaderStatics *iface, HSTRING xaml, IInspectable **result )
{
    struct element *root;
    HRESULT hr;

    TRACE( "iface %p, xaml %s, result %p.\n", iface, debugstr_hstring( xaml ), result );

    if (!result) return E_POINTER;
    if (FAILED(hr = xaml_parse( WindowsGetStringRawBuffer( xaml, NULL ), &root )))
    {
        WARN( "failed to parse XAML, hr %#lx.\n", hr );
        *result = NULL;
        return hr;
    }
    *result = (IInspectable *)&root->IFrameworkElement_iface;
    return S_OK;
}

static HRESULT WINAPI reader_statics_LoadWithInitialTemplateValidation( IXamlReaderStatics *iface, HSTRING xaml,
                                                                         IInspectable **result )
{
    TRACE( "iface %p, xaml %s, result %p.\n", iface, debugstr_hstring( xaml ), result );
    return reader_statics_Load( iface, xaml, result );
}

static const struct IXamlReaderStaticsVtbl reader_statics_vtbl =
{
    reader_statics_QueryInterface,
    reader_statics_AddRef,
    reader_statics_Release,
    /* IInspectable methods */
    reader_statics_GetIids,
    reader_statics_GetRuntimeClassName,
    reader_statics_GetTrustLevel,
    /* IXamlReaderStatics methods */
    reader_statics_Load,
    reader_statics_LoadWithInitialTemplateValidation,
};

static struct reader_statics reader_statics =
{
    {&factory_vtbl},
    {&reader_statics_vtbl},
    1,
};

IActivationFactory *xaml_reader_factory = &reader_statics.IActivationFactory_iface;
