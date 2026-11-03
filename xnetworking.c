/*
 * Xbox Game runtime Library
 *  GDK Component: Networking API -> XNetworking
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

/* for the WINHTTP_FLAG_SECURE_PROTOCOL_* mask the GDK reports below */
#include <winhttp.h>

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_networking
{
    IXNetworkingImpl2 IXNetworkingImpl2_iface;
    LONG ref;
};

static inline struct x_networking *impl_from_IXNetworkingImpl2( IXNetworkingImpl2 *iface )
{
    return CONTAINING_RECORD( iface, struct x_networking, IXNetworkingImpl2_iface );
}

static HRESULT WINAPI x_networking_QueryInterface( IXNetworkingImpl2 *iface, REFIID iid, void **out )
{
    struct x_networking *impl = impl_from_IXNetworkingImpl2( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown          ) ||
        IsEqualGUID( iid, &IID_IXNetworkingImpl  ) ||
        IsEqualGUID( iid, &IID_IXNetworkingImpl2 ))
    {
        IXNetworkingImpl_AddRef( *out = &impl->IXNetworkingImpl2_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_networking_AddRef( IXNetworkingImpl2 *iface )
{
    struct x_networking *impl = impl_from_IXNetworkingImpl2( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_networking_Release( IXNetworkingImpl2 *iface )
{
    struct x_networking *impl = impl_from_IXNetworkingImpl2( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPort( IXNetworkingImpl2 *iface, UINT16 *preferredLocalUdpMultiplayerPort )
{
    TRACE( "iface %p, preferredLocalUdpMultiplayerPort %p.\n", iface, preferredLocalUdpMultiplayerPort );
    if (!preferredLocalUdpMultiplayerPort) return E_INVALIDARG;
    *preferredLocalUdpMultiplayerPort = 3074; /* the standard Xbox Live game port */
    return S_OK;
}

static HRESULT WINAPI x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock )
{
    TRACE( "iface %p, asyncBlock %p.\n", iface, asyncBlock );
    if (!asyncBlock) return E_INVALIDARG;
    xgr_async_complete( asyncBlock, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, UINT16 *preferredLocalUdpMultiplayerPort )
{
    HRESULT hr;
    TRACE( "iface %p, asyncBlock %p, preferredLocalUdpMultiplayerPort %p.\n", iface, asyncBlock, preferredLocalUdpMultiplayerPort );
    if (!preferredLocalUdpMultiplayerPort) return E_INVALIDARG;
    if (!xgr_async_completed( asyncBlock, &hr, NULL )) return E_PENDING;
    *preferredLocalUdpMultiplayerPort = 3074;
    return hr;
}

static HRESULT WINAPI x_networking_XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged( IXNetworkingImpl2 *iface, XTaskQueueHandle queue, void *context, XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );

    /* Connectivity never changes here, so nothing has to be tracked; hand back
     * a non-zero token so the caller has something to unregister. */
    if (token) token->token = 1;
    return S_OK;
}

static BOOLEAN WINAPI x_networking_XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged( IXNetworkingImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, wait %d.\n", iface, wait );
    return TRUE;
}

/* The GDK reports the security requirements a title has to apply to a URL:
 * which HTTP security protocols are enabled, and any certificate thumbprints
 * that must be pinned. This was a stub that returned E_NOTIMPL and never
 * completed the block, and that is what froze multiplayer joins.
 *
 * libHttpClient asks for this while opening the RTA/messaging WebSocket the
 * in-game server list uses (messaging.mktpl.minecraft-services.net). Failing it
 * sends libHttpClient down a connect-failure teardown that essentially never
 * runs on Windows, where the call succeeds: the WebSocket is destroyed while
 * other threads still reference it, so a thread parks for ever on its freed
 * mutex -- and takes the game's main thread with it when that thread wants a
 * mutex the parked one still holds. The same teardown is behind the
 * intermittent read of address 0x8: HC_WEBSOCKET_OBSERVER's destructor erasing
 * itself from the observer map of an already-freed WebSocket. Joining from the
 * history or by IP never opens that socket, which is why only the menu froze.
 *
 * There is nothing to pin: XNetworkingVerifyServerCertificate below accepts the
 * connection, so report no thumbprints and let the caller do the ordinary TLS
 * validation it would otherwise do.
 *
 * The protocol flags are not a formality either. The GDK documents them as a
 * WINHTTP_FLAG_SECURE_PROTOCOL_* mask to be handed straight to
 * WinHttpSetOption( WINHTTP_OPTION_SECURE_PROTOCOLS ), and a title that does
 * exactly that with the zero this used to report tells WinHttp that no TLS
 * version is permitted. Say the modern two instead, which is what a machine
 * with a current policy allows and what every service the titles here talk to
 * negotiates. */
static const XNetworkingSecurityInformation default_security_information =
{
    WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3,
};

static HRESULT security_information_result( XAsyncBlock *async, SIZE_T buffer_size, SIZE_T *buffer_used,
                                            UINT8 *buffer, XNetworkingSecurityInformation **ptr_to_buffer )
{
    void *payload;
    HRESULT hr;

    if (!async) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, &payload )) return E_PENDING;
    if (FAILED(hr)) return hr;

    if (buffer_used) *buffer_used = sizeof(default_security_information);
    if (!buffer) return S_OK;
    if (buffer_size < sizeof(default_security_information)) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );

    memcpy( buffer, &default_security_information, sizeof(default_security_information) );
    if (ptr_to_buffer) *ptr_to_buffer = (XNetworkingSecurityInformation *)buffer;
    return S_OK;
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlAsync( IXNetworkingImpl2 *iface, const char *url, XAsyncBlock *asyncBlock )
{
    TRACE( "iface %p, url %s, asyncBlock %p.\n", iface, debugstr_a(url), asyncBlock );

    if (!asyncBlock) return E_INVALIDARG;
    xgr_async_complete_ex( asyncBlock, S_OK, (void *)&default_security_information,
                           sizeof(default_security_information) );
    return S_OK;
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResultSize( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T *securityInformationBufferByteCount )
{
    TRACE( "iface %p, asyncBlock %p, securityInformationBufferByteCount %p.\n", iface, asyncBlock, securityInformationBufferByteCount );
    return security_information_result( asyncBlock, 0, securityInformationBufferByteCount, NULL, NULL );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResult( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T securityInformationBufferByteCount, SIZE_T *securityInformationBufferByteCountUsed, UINT8 *securityInformationBuffer, XNetworkingSecurityInformation **securityInformation )
{
    TRACE( "iface %p, asyncBlock %p, securityInformationBufferByteCount %Iu, buffer %p.\n",
           iface, asyncBlock, securityInformationBufferByteCount, securityInformationBuffer );
    return security_information_result( asyncBlock, securityInformationBufferByteCount,
                                        securityInformationBufferByteCountUsed,
                                        securityInformationBuffer, securityInformation );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlUtf16Async( IXNetworkingImpl2 *iface, const WCHAR *url, XAsyncBlock *asyncBlock )
{
    TRACE( "iface %p, url %s, asyncBlock %p.\n", iface, debugstr_w(url), asyncBlock );

    if (!asyncBlock) return E_INVALIDARG;
    xgr_async_complete_ex( asyncBlock, S_OK, (void *)&default_security_information,
                           sizeof(default_security_information) );
    return S_OK;
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T *securityInformationBufferByteCount )
{
    TRACE( "iface %p, asyncBlock %p, securityInformationBufferByteCount %p.\n", iface, asyncBlock, securityInformationBufferByteCount );
    return security_information_result( asyncBlock, 0, securityInformationBufferByteCount, NULL, NULL );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T securityInformationBufferByteCount, SIZE_T *securityInformationBufferByteCountUsed, UINT8 *securityInformationBuffer, XNetworkingSecurityInformation **securityInformation )
{
    TRACE( "iface %p, asyncBlock %p, securityInformationBufferByteCount %Iu, buffer %p.\n",
           iface, asyncBlock, securityInformationBufferByteCount, securityInformationBuffer );
    return security_information_result( asyncBlock, securityInformationBufferByteCount,
                                        securityInformationBufferByteCountUsed,
                                        securityInformationBuffer, securityInformation );
}

static HRESULT WINAPI x_networking_XNetworkingVerifyServerCertificate( IXNetworkingImpl2 *iface, void *requestHandle, const XNetworkingSecurityInformation *securityInformation )
{
    TRACE( "iface %p, requestHandle %p, securityInformation %p.\n", iface, requestHandle, securityInformation );
    /* no Xbox Live cert pinning offline; accept the connection */
    return S_OK;
}

/* Report a plain, unmetered wired connection. Titles gate HTTP startup on this:
 * XCurl refuses to initialise when the runtime reports no network, which
 * surfaces as a fatal "Could not initialize libcurl" during engine init rather
 * than as a networking error later. */
static void fill_connectivity_hint( XNetworkingConnectivityHint *hint )
{
    hint->connectivityLevel = XNetworkingConnectivityLevelHint_InternetAccess;
    hint->connectivityCost = XNetworkingConnectivityCostHint_Unrestricted;
    hint->ianaInterfaceType = 6; /* IF_TYPE_ETHERNET_CSMACD */
    hint->networkInitialized = TRUE;
    hint->approachingDataLimit = FALSE;
    hint->overDataLimit = FALSE;
    hint->roaming = FALSE;
}

/* Registering for connectivity changes delivers the current hint once, straight
 * away. That first callback is not a nicety: libHttpClient caches
 * networkInitialized from it, and refuses every WebSocket connect while the
 * cached value is false -- plain HTTP never looks at it, which is why sign-in,
 * profile and Realms all worked while anything WebSocket-shaped did not.
 *
 * Minecraft's server list needs two WebSockets (Xbox Live's Real-Time Activity
 * socket and the multiplayer signalling socket), so with no initial callback
 * every join from the menu failed inside libHttpClient's connect path. Its
 * connect-failure teardown then destroys the WebSocket while an observer still
 * references it, and the observer's destructor later touches freed memory: that
 * is both the intermittent "read of address 0x8" crash and the freeze where a
 * thread parks for ever on a mutex whose memory has been reused.
 *
 * It goes out on the thread pool rather than inline: the caller registers from
 * inside its own network-state lock, and its handler takes that same
 * non-recursive lock. */
struct connectivity_notification
{
    void *context;
    XNetworkingConnectivityHintChangedCallback *callback;
};

static void CALLBACK connectivity_notification_thunk( TP_CALLBACK_INSTANCE *instance, void *param )
{
    struct connectivity_notification *item = param;
    XNetworkingConnectivityHint hint;

    fill_connectivity_hint( &hint );
    item->callback( item->context, &hint );
    free( item );
}

static DWORD WINAPI connectivity_notification_thread( void *param )
{
    connectivity_notification_thunk( NULL, param );
    return 0;
}

static void notify_connectivity_hint( void *context, XNetworkingConnectivityHintChangedCallback *callback )
{
    struct connectivity_notification *item;
    HANDLE thread;

    if (!callback) return;
    if (!(item = calloc( 1, sizeof(*item) ))) return;
    item->context = context;
    item->callback = callback;

    if (TrySubmitThreadpoolCallback( connectivity_notification_thunk, item, NULL )) return;

    /* Dropping it would leave the title believing it has no network for the
     * rest of the run, so spend a thread instead. */
    if ((thread = CreateThread( NULL, 0, connectivity_notification_thread, item, 0, NULL )))
    {
        CloseHandle( thread );
        return;
    }
    ERR( "could not deliver the initial connectivity hint.\n" );
    free( item );
}

static HRESULT WINAPI x_networking_XNetworkingGetConnectivityHint( IXNetworkingImpl2 *iface, XNetworkingConnectivityHint *connectivityHint )
{
    TRACE( "iface %p, connectivityHint %p.\n", iface, connectivityHint );

    if (!connectivityHint) return E_INVALIDARG;

    fill_connectivity_hint( connectivityHint );
    return S_OK;
}

static HRESULT WINAPI x_networking_XNetworkingRegisterConnectivityHintChanged( IXNetworkingImpl2 *iface, XTaskQueueHandle queue, void *context, XNetworkingConnectivityHintChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );

    if (token) token->token = 1;
    notify_connectivity_hint( context, callback );
    return S_OK;
}

static BOOLEAN WINAPI x_networking_XNetworkingUnregisterConnectivityHintChanged( IXNetworkingImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, token %#I64x, wait %d.\n", iface, token.token, wait );
    return TRUE;
}

static HRESULT WINAPI x_networking_XNetworkingQueryConfigurationSetting( IXNetworkingImpl2 *iface, XNetworkingConfigurationSetting configurationSetting, UINT64 *value )
{
    TRACE( "iface %p, configurationSetting %d, value %p.\n", iface, configurationSetting, value );
    if (!value) return E_INVALIDARG;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI x_networking_XNetworkingSetConfigurationSetting( IXNetworkingImpl2 *iface, XNetworkingConfigurationSetting configurationParameter, UINT64 value )
{
    TRACE( "iface %p, configurationParameter %d, value %I64u.\n", iface, configurationParameter, value );
    return S_OK;
}

static HRESULT WINAPI x_networking_XNetworkingQueryStatistics( IXNetworkingImpl2 *iface, XNetworkingStatisticsType statisticsType, XNetworkingStatisticsBuffer *statisticsBuffer )
{
    TRACE( "iface %p, statisticsType %d, statisticsBuffer %p.\n", iface, statisticsType, statisticsBuffer );
    if (statisticsBuffer) memset( statisticsBuffer, 0, sizeof(*statisticsBuffer) );
    return S_OK;
}

static const struct IXNetworkingImpl2Vtbl x_networking_vtbl =
{
    x_networking_QueryInterface,
    x_networking_AddRef,
    x_networking_Release,
    /* IXNetworkingImpl methods */
    x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPort,
    x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync,
    x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult,
    x_networking_XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged,
    x_networking_XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged,
    x_networking_XNetworkingQuerySecurityInformationForUrlAsync,
    x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResultSize,
    x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResult,
    x_networking_XNetworkingQuerySecurityInformationForUrlUtf16Async,
    x_networking_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize,
    x_networking_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult,
    x_networking_XNetworkingVerifyServerCertificate,
    x_networking_XNetworkingGetConnectivityHint,
    x_networking_XNetworkingRegisterConnectivityHintChanged,
    x_networking_XNetworkingUnregisterConnectivityHintChanged,
    /* IXNetworkingImpl2 methods */
    x_networking_XNetworkingQueryConfigurationSetting,
    x_networking_XNetworkingSetConfigurationSetting,
    x_networking_XNetworkingQueryStatistics,
};

static struct x_networking x_networking =
{
    {&x_networking_vtbl},
    0,
};

IXNetworkingImpl *x_networking_impl = (IXNetworkingImpl *)&x_networking.IXNetworkingImpl2_iface;
