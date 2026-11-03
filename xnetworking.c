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

/* for the WINHTTP_FLAG_SECURE_PROTOCOL_* mask the GDK reports below, and the
 * request handles XNetworkingVerifyServerCertificate is given */
#include <winhttp.h>
#include <wincrypt.h>

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
 * There is nothing to pin: report no thumbprints, and let
 * XNetworkingVerifyServerCertificate below do the ordinary TLS validation.
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

/* XNetworkingVerifyServerCertificate.
 *
 * A title (libHttpClient's WinHTTP provider, in practice) calls this from its
 * WINHTTP_CALLBACK_STATUS_SENDING_REQUEST callback with the request handle, and
 * fails the request unless it succeeds. It used to succeed unconditionally,
 * which made it no check at all. So the server's certificate is taken from the
 * request and its chain verified against the request's host, under the SSL
 * policy, the way WinHTTP verifies it while connecting -- including the
 * SECURITY_FLAG_IGNORE_* exceptions the request carries, so a request WinHTTP
 * was told to accept is accepted here too.
 *
 * Neither library is imported: the handle can only be a WinHTTP request if the
 * title has winhttp.dll loaded, and crypt32.dll is loaded by it. A handle that
 * is not a WinHTTP request, or a request with no server certificate (plain
 * HTTP), cannot be checked here; that keeps the old answer, logged once.
 *
 * Thumbprint pinning is not checked. QuerySecurityInformationForUrl above
 * reports none, so a title only has some to pass if it built its own. */
static INIT_ONCE crypt32_once = INIT_ONCE_STATIC_INIT;
static __typeof__(CertGetCertificateChain) *pCertGetCertificateChain;
static __typeof__(CertVerifyCertificateChainPolicy) *pCertVerifyCertificateChainPolicy;
static __typeof__(CertFreeCertificateChain) *pCertFreeCertificateChain;
static __typeof__(CertFreeCertificateContext) *pCertFreeCertificateContext;

static BOOL WINAPI load_crypt32( INIT_ONCE *once, void *param, void **context )
{
    HMODULE crypt32 = LoadLibraryW( L"crypt32.dll" );

    if (!crypt32) return TRUE;
    pCertGetCertificateChain = (void *)GetProcAddress( crypt32, "CertGetCertificateChain" );
    pCertVerifyCertificateChainPolicy = (void *)GetProcAddress( crypt32, "CertVerifyCertificateChainPolicy" );
    pCertFreeCertificateChain = (void *)GetProcAddress( crypt32, "CertFreeCertificateChain" );
    pCertFreeCertificateContext = (void *)GetProcAddress( crypt32, "CertFreeCertificateContext" );
    return TRUE;
}

static void cannot_verify( void *request, const char *why, DWORD error )
{
    static LONG logged;

    if (!InterlockedExchange( &logged, 1 ))
        WARN( "cannot verify the server certificate of %p (%s, error %lu); accepting it, as before.\n",
              request, why, error );
    else TRACE( "cannot verify the server certificate of %p (%s, error %lu).\n", request, why, error );
}

/* The host the request was made to, from its URL. */
static BOOL request_host( __typeof__(WinHttpQueryOption) *query, __typeof__(WinHttpCrackUrl) *crack,
                          HINTERNET request, WCHAR *host, DWORD host_len )
{
    URL_COMPONENTS parts = { sizeof(parts) };
    DWORD size = 0;
    WCHAR *url;
    BOOL ret = FALSE;

    query( request, WINHTTP_OPTION_URL, NULL, &size );
    if (!size || !(url = calloc( 1, size + sizeof(WCHAR) ))) return FALSE;
    parts.dwHostNameLength = ~0u;
    if (query( request, WINHTTP_OPTION_URL, url, &size ) && crack( url, 0, 0, &parts ) &&
        parts.lpszHostName && parts.dwHostNameLength && parts.dwHostNameLength < host_len)
    {
        memcpy( host, parts.lpszHostName, parts.dwHostNameLength * sizeof(WCHAR) );
        host[parts.dwHostNameLength] = 0;
        ret = TRUE;
    }
    free( url );
    return ret;
}

/* The chain errors a request can carry, and the exception that lets it through.
 * Each is weighed on its own: an ignored error must not hide one that is not,
 * as it did when they were checked in turn (and as dlls/winhttp/net.c still
 * does), so an expired certificate the request accepted from an unknown CA it
 * did not accept was let through. Revocation has no exception. */
static const struct
{
    DWORD trust_errors;
    DWORD ignore;
    DWORD error;
}
chain_errors[] =
{
    { CERT_TRUST_IS_NOT_TIME_VALID, SECURITY_FLAG_IGNORE_CERT_DATE_INVALID, ERROR_WINHTTP_SECURE_CERT_DATE_INVALID },
    { CERT_TRUST_IS_UNTRUSTED_ROOT | CERT_TRUST_IS_PARTIAL_CHAIN, SECURITY_FLAG_IGNORE_UNKNOWN_CA,
      ERROR_WINHTTP_SECURE_INVALID_CA },
    { CERT_TRUST_IS_REVOKED, 0, ERROR_WINHTTP_SECURE_CERT_REVOKED },
    { CERT_TRUST_IS_NOT_VALID_FOR_USAGE, SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE, ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE },
};

/* Revocation is looked up in the caches only (below), so not knowing is the
 * usual answer, not a reason to refuse. */
static const DWORD revocation_unknown = CERT_TRUST_IS_OFFLINE_REVOCATION | CERT_TRUST_REVOCATION_STATUS_UNKNOWN;

/* WinHTTP's verdict: chain errors the request does not ignore first, then the
 * SSL policy for the host.
 *
 * Revocation is checked from what the system already knows -- a status cached
 * by an earlier online check, a CRL in the URL cache, or a local distribution
 * point -- and never fetched: this runs on the title's thread, in the middle of
 * sending its request. */
static DWORD verify_chain( const CERT_CONTEXT *cert, WCHAR *host, DWORD security_flags )
{
    char oid_server_auth[] = szOID_PKIX_KP_SERVER_AUTH;
    char *server_auth[] = { oid_server_auth };
    CERT_CHAIN_PARA chain_para = { sizeof(chain_para) };
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl = { {sizeof(ssl)} };
    CERT_CHAIN_POLICY_PARA policy = { sizeof(policy) };
    CERT_CHAIN_POLICY_STATUS status = { sizeof(status) };
    const CERT_CHAIN_CONTEXT *chain;
    CERT_CHAIN_CONTEXT copy;
    DWORD errors, known = revocation_unknown, ignored, err = ERROR_SUCCESS;
    unsigned int i;

    chain_para.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    chain_para.RequestedUsage.Usage.cUsageIdentifier = 1;
    chain_para.RequestedUsage.Usage.rgpszUsageIdentifier = server_auth;
    if (!pCertGetCertificateChain( NULL, cert, NULL, cert->hCertStore, &chain_para,
                                   CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT |
                                   CERT_CHAIN_REVOCATION_CHECK_CACHE_ONLY, NULL, &chain ))
        return ERROR_WINHTTP_SECURE_CHANNEL_ERROR;

    errors = chain->TrustStatus.dwErrorStatus;
    ignored = errors & revocation_unknown;
    for (i = 0; i < ARRAY_SIZE(chain_errors); i++)
    {
        known |= chain_errors[i].trust_errors;
        if (!(errors & chain_errors[i].trust_errors)) continue;
        if (chain_errors[i].ignore && (security_flags & chain_errors[i].ignore))
            ignored |= errors & chain_errors[i].trust_errors;
        else if (!err) err = chain_errors[i].error;
    }
    if (!err && (errors & ~known)) err = ERROR_WINHTTP_SECURE_INVALID_CERT;
    if (err) TRACE( "chain errors %#lx, ignoring %#lx: %lu.\n", errors, ignored, err );

    if (!err)
    {
        /* Only what was ignored above is hidden from the policy, so it still
         * makes its other checks, the host name among them. */
        copy = *chain;
        copy.TrustStatus.dwErrorStatus &= ~ignored;
        ssl.dwAuthType = AUTHTYPE_SERVER;
        ssl.pwszServerName = host;
        ssl.fdwChecks = security_flags;
        policy.pvExtraPolicyPara = &ssl;
        if (!pCertVerifyCertificateChainPolicy( CERT_CHAIN_POLICY_SSL, &copy, &policy, &status ))
            err = ERROR_WINHTTP_SECURE_CHANNEL_ERROR;
        else if (status.dwError == CERT_E_CN_NO_MATCH) err = ERROR_WINHTTP_SECURE_CERT_CN_INVALID;
        else if (status.dwError) err = ERROR_WINHTTP_SECURE_INVALID_CERT;
    }
    pCertFreeCertificateChain( chain );
    return err;
}

static HRESULT WINAPI x_networking_XNetworkingVerifyServerCertificate( IXNetworkingImpl2 *iface, void *requestHandle, const XNetworkingSecurityInformation *securityInformation )
{
    static const DWORD ignorable = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                                   SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    __typeof__(WinHttpQueryOption) *query;
    __typeof__(WinHttpCrackUrl) *crack;
    const CERT_CONTEXT *cert = NULL;
    DWORD size, flags = 0, err;
    WCHAR host[256] = {0};
    HMODULE winhttp;
    HRESULT hr;

    TRACE( "iface %p, requestHandle %p, securityInformation %p.\n", iface, requestHandle, securityInformation );

    if (securityInformation && securityInformation->thumbprintCount)
        FIXME( "%Iu certificate thumbprints not checked.\n", securityInformation->thumbprintCount );

    InitOnceExecuteOnce( &crypt32_once, load_crypt32, NULL, NULL );
    if (!pCertGetCertificateChain || !pCertVerifyCertificateChainPolicy || !pCertFreeCertificateChain ||
        !pCertFreeCertificateContext)
    {
        cannot_verify( requestHandle, "no crypt32", 0 );
        return S_OK;
    }
    /* Held for the call, so the title cannot unload it from under us. */
    if (!requestHandle || !GetModuleHandleExW( 0, L"winhttp.dll", &winhttp ))
    {
        cannot_verify( requestHandle, "not a WinHTTP request", 0 );
        return S_OK;
    }
    query = (void *)GetProcAddress( winhttp, "WinHttpQueryOption" );
    crack = (void *)GetProcAddress( winhttp, "WinHttpCrackUrl" );
    size = sizeof(cert);
    if (!query || !crack || !query( requestHandle, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &size ) || !cert)
    {
        cannot_verify( requestHandle, "no server certificate", GetLastError() );
        FreeLibrary( winhttp );
        return S_OK;
    }
    size = sizeof(flags);
    if (!query( requestHandle, WINHTTP_OPTION_SECURITY_FLAGS, &flags, &size )) flags = 0;
    flags &= ignorable;

    if (!request_host( query, crack, requestHandle, host, ARRAY_SIZE(host) ))
    {
        /* The certificate is from a connection WinHTTP made to some host; with
         * no way to name it, the name check has nothing to compare against. */
        WARN( "no host for request %p; verifying the chain without its name.\n", requestHandle );
        err = verify_chain( cert, NULL, flags | SECURITY_FLAG_IGNORE_CERT_CN_INVALID );
    }
    else err = verify_chain( cert, host, flags );

    pCertFreeCertificateContext( cert );
    FreeLibrary( winhttp );

    hr = err ? HRESULT_FROM_WIN32( err ) : S_OK;
    if (FAILED(hr)) WARN( "server certificate of %p for %s refused: %#lx.\n", requestHandle, debugstr_w( host ), hr );
    else TRACE( "server certificate of %p for %s verified (ignoring %#lx).\n", requestHandle, debugstr_w( host ), flags );
    return hr;
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
