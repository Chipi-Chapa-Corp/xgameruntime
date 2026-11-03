/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XSystem
 *
 * Written by Weather
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
#include "winternl.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_system_analytics
{
    IXSystemAnalyticsImpl IXSystemAnalyticsImpl_iface;
    LONG ref;
};

static inline struct x_system_analytics *impl_from_IXSystemAnalyticsImpl( IXSystemAnalyticsImpl *iface )
{
    return CONTAINING_RECORD( iface, struct x_system_analytics, IXSystemAnalyticsImpl_iface );
}

static HRESULT WINAPI x_system_analytics_QueryInterface( IXSystemAnalyticsImpl *iface, REFIID iid, void **out )
{
    struct x_system_analytics *impl = impl_from_IXSystemAnalyticsImpl( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown              ) ||
        IsEqualGUID( iid, &IID_IXSystemAnalyticsImpl ))
    {
        IXSystemAnalyticsImpl_AddRef( *out = &impl->IXSystemAnalyticsImpl_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_system_analytics_AddRef( IXSystemAnalyticsImpl *iface )
{
    struct x_system_analytics *impl = impl_from_IXSystemAnalyticsImpl( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_system_analytics_Release( IXSystemAnalyticsImpl *iface )
{
    struct x_system_analytics *impl = impl_from_IXSystemAnalyticsImpl( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

/* What this machine is, without asking WinRT.
 *
 * Every failure path below used to `return NULL`, and that is not a thing this
 * function may do. It returns a struct *by value*: the caller passes the buffer
 * in `__ret` and expects the same pointer back in rax, then copies the struct
 * out of it. Goat Simulator 3 does exactly that --
 *
 *     call rax             ; XSystemGetAnalyticsInfo
 *     movups xmm0,[rax]    ; copy the 32 bytes it returned
 *
 * -- so returning NULL is a null dereference in the title, one instruction
 * later, with nothing in the log to say why.
 *
 * The answer is also not a guess: on Windows this reports the Desktop device
 * family, which is what the successful path derives by splitting
 * "Windows.Desktop" at the dot, and the version is the one the OS reports. */
static void analytics_info_fallback( XSystemAnalyticsInfo *info )
{
    RTL_OSVERSIONINFOEXW version = { .dwOSVersionInfoSize = sizeof(version) };

    memset( info, 0, sizeof(*info) );
    strcpy( info->family, "Windows.Desktop" );
    strcpy( info->form, "Desktop" );

    if (!RtlGetVersion( &version ))
    {
        info->osVersion.major = version.dwMajorVersion;
        info->osVersion.minor = version.dwMinorVersion;
        info->osVersion.build = version.dwBuildNumber;
        info->osVersion.revision = 0;
    }
    info->hostingOsVersion = info->osVersion;
}

static XSystemAnalyticsInfo* WINAPI x_system_analytics_XSystemGetAnalyticsInfo( IXSystemAnalyticsImpl *iface, XSystemAnalyticsInfo *__ret )
{
    /* For Windows, XSystemAnalyticsInfo->form is always "Desktop" */
    const WCHAR *analytics_info_str = RuntimeClass_Windows_System_Profile_AnalyticsInfo;
    HSTRING analytics_info_class, deviceFamilyVersion, deviceFamily;
    const WCHAR *deviceFamilyVersionStr, *deviceFamilyStr;
    XSystemAnalyticsInfo info = {0};
    char *str;
    ULONGLONG version;
    HRESULT status;
    UINT32 strSize;

    IAnalyticsInfoStatics *analytics_info_statics = NULL;
    IAnalyticsVersionInfo *analytics_version_info = NULL;

    TRACE( "iface %p.\n", iface );

    status = WindowsCreateString( analytics_info_str, wcslen( analytics_info_str ), &analytics_info_class );
    if (FAILED( status )) goto fallback;

    status = RoGetActivationFactory( analytics_info_class, &IID_IAnalyticsInfoStatics, (void **)&analytics_info_statics );
    WindowsDeleteString( analytics_info_class );
    if (FAILED( status )) goto fallback;

    status = IAnalyticsInfoStatics_get_VersionInfo( analytics_info_statics, &analytics_version_info );
    IAnalyticsInfoStatics_Release( analytics_info_statics );
    if (FAILED( status )) goto fallback;

    status = IAnalyticsVersionInfo_get_DeviceFamilyVersion( analytics_version_info, &deviceFamilyVersion );
    if (FAILED( status ))
    {
        IAnalyticsVersionInfo_Release( analytics_version_info );
        goto fallback;
    }

    status = IAnalyticsVersionInfo_get_DeviceFamily( analytics_version_info, &deviceFamily );
    IAnalyticsVersionInfo_Release( analytics_version_info );
    if (FAILED( status ))
    {
        WindowsDeleteString( deviceFamilyVersion );
        goto fallback;
    }

    deviceFamilyStr = WindowsGetStringRawBuffer( deviceFamily, NULL );
    strSize = WideCharToMultiByte( CP_UTF8, 0, deviceFamilyStr, -1, NULL, 0, NULL, NULL );

    str = (char *)malloc( strSize );
    if (!str)
    {
        WindowsDeleteString( deviceFamilyVersion );
        WindowsDeleteString( deviceFamily );
        goto fallback;
    }

    if (!WideCharToMultiByte( CP_UTF8, 0, deviceFamilyStr, -1, str, strSize, NULL, NULL ))
    {
        WindowsDeleteString( deviceFamilyVersion );
        WindowsDeleteString( deviceFamily );
        free( str );
        goto fallback;
    }

    /* GDK consumers classify the complete device family, e.g.
     * Windows.Desktop; splitting it makes PlayFab Party see an unknown OS. */
    if (strSize > sizeof(info.family))
    {
        WindowsDeleteString( deviceFamilyVersion );
        WindowsDeleteString( deviceFamily );
        free( str );
        goto fallback;
    }
    memcpy( info.family, str, strSize );
    strcpy( info.form, "Desktop" );

    WindowsDeleteString( deviceFamily );
    free( str );

    deviceFamilyVersionStr = WindowsGetStringRawBuffer( deviceFamilyVersion, NULL );
    strSize = WideCharToMultiByte( CP_UTF8, 0, deviceFamilyVersionStr, -1, NULL, 0, NULL, NULL );

    str = (char *)malloc( strSize );
    if (!str)
    {
        WindowsDeleteString( deviceFamilyVersion );
        goto fallback;
    }

    if (!WideCharToMultiByte( CP_UTF8, 0, deviceFamilyVersionStr, -1, str, strSize, NULL, NULL ))
    {
        WindowsDeleteString( deviceFamilyVersion );
        free( str );
        goto fallback;
    }

    version = strtoull( str, NULL, 10 );
    info.osVersion.major = (UINT16)(version >> 48);
    info.osVersion.minor = (UINT16)((version >> 32) & 0xFFFF);
    info.osVersion.build = (UINT16)((version >> 16) & 0xFFFF);
    info.osVersion.revision = (UINT16)(version & 0xFFFF);

    WindowsDeleteString( deviceFamilyVersion );
    free( str );

    info.hostingOsVersion = info.osVersion;

    TRACE( "returning complete analytics device family %s.\n", debugstr_a(info.family) );
    *__ret = info;

    return __ret;

fallback:
    analytics_info_fallback( __ret );
    TRACE( "returning complete analytics device family %s.\n", debugstr_a(__ret->family) );
    return __ret;
}

static const struct IXSystemAnalyticsImplVtbl x_system_analytics_vtbl =
{
    x_system_analytics_QueryInterface,
    x_system_analytics_AddRef,
    x_system_analytics_Release,
    /* IXSystemAnalyticsImpl methods */
    x_system_analytics_XSystemGetAnalyticsInfo,
};

static struct x_system_analytics x_system_analytics =
{
    {&x_system_analytics_vtbl},
    0,
};

IXSystemAnalyticsImpl *x_system_analytics_impl = &x_system_analytics.IXSystemAnalyticsImpl_iface;
