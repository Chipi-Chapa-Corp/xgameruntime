/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XUser
 *
 * Copyright 2026 Olivia Ryan
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
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_user
{
    IXUserImpl6 IXUserImpl6_iface;
    IXUserGamertagImpl IXUserGamertagImpl_iface;
    IXUserDeviceImpl2 IXUserDeviceImpl2_iface;
    LONG ref;
};

static inline struct x_user *impl_from_IXUserImpl6( IXUserImpl6 *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserImpl6_iface );
}

static HRESULT WINAPI x_user_QueryInterface( IXUserImpl6 *iface, REFIID iid, void **out )
{
    struct x_user *impl = impl_from_IXUserImpl6( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown    ) ||
        IsEqualGUID( iid, &IID_IXUserImpl  ) ||
        IsEqualGUID( iid, &IID_IXUserImpl2 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl3 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl4 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl5 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl6 ))
    {
        IXUserImpl6_AddRef( *out = &impl->IXUserImpl6_iface );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IXUserGamertagImpl ))
    {
        IXUserGamertagImpl_AddRef( *out = &impl->IXUserGamertagImpl_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_user_AddRef( IXUserImpl6 *iface )
{
    struct x_user *impl = impl_from_IXUserImpl6( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_user_Release( IXUserImpl6 *iface )
{
    struct x_user *impl = impl_from_IXUserImpl6( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

/* A single local user.
 *
 * There is no Xbox Live sign-in available here, so the runtime presents one
 * persistent offline user rather than failing sign-in outright: titles call
 * XUserAddAsync during startup and treat failure as fatal. The handle is a
 * fixed singleton, which also makes XUserCompare and the FindUserBy* lookups
 * trivially consistent.
 */
struct x_local_user
{
    LONG ref;
    UINT64 id;
    XUserLocalId local_id;
};

static struct x_local_user local_user =
{
    .ref = 1,
    .id = 0,
    .local_id = { .value = 1 },
};

static XUserHandle local_user_handle( void )
{
    return (XUserHandle)&local_user;
}

static BOOL is_local_user( XUserHandle user )
{
    return user == local_user_handle();
}

/* Offline there is no real XUID to report. XGR_XUID lets the launcher supply
 * the one the user's Windows install used, so on-disk layouts keyed by it --
 * the game save folders in particular -- line up with what Windows wrote. */
UINT64 xgr_local_xuid( void )
{
    static UINT64 xuid;
    static BOOL initialized;
    WCHAR buf[32], *p;

    if (initialized) return xuid;
    if (GetEnvironmentVariableW( L"XGR_XUID", buf, ARRAY_SIZE(buf) ))
    {
        for (p = buf; *p; p++)
        {
            UINT64 digit;
            if (*p >= '0' && *p <= '9') digit = *p - '0';
            else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
            else if (*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
            else break;
            xuid = (xuid << 4) | digit;
        }
    }
    initialized = TRUE;
    return xuid;
}

/* The signed-in account's real identity.
 *
 * An Xbox Live token carries the account it was issued to, so one request
 * against the Xbox Live relying party yields the XUID and gamertag to report
 * instead of a placeholder -- without which a title has no idea who is playing
 * and shows nobody as signed in. Resolved off whichever thread asks first, and
 * only latched once it succeeds: a single transient failure used to leave the
 * process offline for the rest of its life, which showed up as being signed out
 * until the title was restarted. */
static struct
{
    UINT64 xuid;
    char gamertag[XGR_IDENTITY_MAX];
} live_identity;

static INIT_ONCE live_identity_once = INIT_ONCE_STATIC_INIT;

static BOOL WINAPI resolve_live_identity( INIT_ONCE *once, void *param, void **ctx )
{
    struct xsts_token_params params = {0};
    const char *p;

    /* Any Xbox Live relying party returns the display claims; this one is the
     * canonical choice and is what the account's own services use. */
    params.url = "https://xboxlive.com";
    if (XGR_UNIX_CALL( xsts_token, &params ))
    {
        /* FALSE leaves the once uninitialised, so the next caller tries again
         * rather than the whole process staying offline over one failure. */
        WARN( "no Xbox Live identity yet; will try again on the next request.\n" );
        return FALSE;
    }

    for (p = params.xuid; *p >= '0' && *p <= '9'; p++)
        live_identity.xuid = live_identity.xuid * 10 + (*p - '0');
    memcpy( live_identity.gamertag, params.gamertag, sizeof(live_identity.gamertag) );
    live_identity.gamertag[sizeof(live_identity.gamertag) - 1] = 0;

    TRACE( "signed in as %s (xuid %I64u).\n", debugstr_a( live_identity.gamertag ), live_identity.xuid );
    return TRUE;
}

static void ensure_live_identity( void )
{
    InitOnceExecuteOnce( &live_identity_once, resolve_live_identity, NULL, NULL );
}

static HRESULT WINAPI x_user_XUserDuplicateHandle( IXUserImpl6 *iface, XUserHandle handle, XUserHandle *duplicatedHandle )
{
    TRACE( "iface %p, handle %p, duplicatedHandle %p.\n", iface, handle, duplicatedHandle );

    if (!is_local_user( handle ) || !duplicatedHandle) return E_INVALIDARG;
    InterlockedIncrement( &local_user.ref );
    *duplicatedHandle = handle;
    return S_OK;
}

static void WINAPI x_user_XUserCloseHandle( IXUserImpl6 *iface, XUserHandle user )
{
    TRACE( "iface %p, user %p.\n", iface, user );

    /* The local user is statically allocated and outlives every handle. */
    if (is_local_user( user )) InterlockedDecrement( &local_user.ref );
}

static INT32 WINAPI x_user_XUserCompare( IXUserImpl6 *iface, XUserHandle user1, XUserHandle user2 )
{
    TRACE( "iface %p, user1 %p, user2 %p.\n", iface, user1, user2 );

    if (user1 == user2) return 0;
    return user1 < user2 ? -1 : 1;
}

static HRESULT WINAPI x_user_XUserGetMaxUsers( IXUserImpl6 *iface, UINT32 *maxUsers )
{
    TRACE( "iface %p, maxUsers %p.\n", iface, maxUsers );
    *maxUsers = 1;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserAddAsync( IXUserImpl6 *iface, XUserAddOptions options, XAsyncBlock *async )
{
    TRACE( "iface %p, options %d, async %p.\n", iface, options, async );

    if (!async) return E_INVALIDARG;

    /* Nothing to negotiate for a local user, so complete before returning. */
    xgr_async_complete( async, S_OK, local_user_handle() );
    return S_OK;
}

static HRESULT WINAPI x_user_XUserAddResult( IXUserImpl6 *iface, XAsyncBlock *async, XUserHandle *newUser )
{
    void *payload;
    HRESULT hr;

    TRACE( "iface %p, async %p, newUser %p.\n", iface, async, newUser );

    if (!async || !newUser) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, &payload )) return E_PENDING;
    if (FAILED(hr)) return hr;

    InterlockedIncrement( &local_user.ref );
    *newUser = payload;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserGetLocalId( IXUserImpl6 *iface, XUserHandle user, XUserLocalId *userLocalId )
{
    TRACE( "iface %p, user %p, userLocalId %p.\n", iface, user, userLocalId );

    if (!is_local_user( user ) || !userLocalId) return E_INVALIDARG;
    *userLocalId = local_user.local_id;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserFindUserByLocalId( IXUserImpl6 *iface, XUserLocalId userLocalId, XUserHandle *handle )
{
    TRACE( "iface %p, userLocalId %#I64x, handle %p.\n", iface, userLocalId.value, handle );

    if (!handle) return E_INVALIDARG;
    if (userLocalId.value != local_user.local_id.value) return E_FAIL;
    InterlockedIncrement( &local_user.ref );
    *handle = local_user_handle();
    return S_OK;
}

static HRESULT WINAPI x_user_XUserGetId( IXUserImpl6 *iface, XUserHandle user, UINT64 *userId )
{
    TRACE( "iface %p, user %p, userId %p.\n", iface, user, userId );

    if (!is_local_user( user ) || !userId) return E_INVALIDARG;

    /* Prefer the XUID the Xbox Live token was issued to; XGR_XUID (the one the
     * user's Windows install saved under) remains the offline fallback so
     * on-disk save layouts still line up. */
    ensure_live_identity();
    if (live_identity.xuid) *userId = live_identity.xuid;
    else *userId = local_user.id ? local_user.id : xgr_local_xuid();
    return S_OK;
}

static HRESULT WINAPI x_user_XUserFindUserById( IXUserImpl6 *iface, UINT64 userId, XUserHandle *handle )
{
    TRACE( "iface %p, userId %I64u, handle %p.\n", iface, userId, handle );
    if (!handle) return E_INVALIDARG;
    *handle = local_user_handle();
    return S_OK;
}

static HRESULT WINAPI x_user_XUserGetIsGuest( IXUserImpl6 *iface, XUserHandle user, BOOLEAN *isGuest )
{
    TRACE( "iface %p, user %p, isGuest %p.\n", iface, user, isGuest );

    if (!is_local_user( user ) || !isGuest) return E_INVALIDARG;
    *isGuest = FALSE;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserGetState( IXUserImpl6 *iface, XUserHandle user, XUserState *state )
{
    TRACE( "iface %p, user %p, state %p.\n", iface, user, state );

    if (!is_local_user( user ) || !state) return E_INVALIDARG;
    *state = XUserState_SignedIn;
    return S_OK;
}

static HRESULT WINAPI __PADDING__( IXUserImpl6 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureAsync( IXUserImpl6 *iface, XUserHandle user, XUserGamerPictureSize pictureSize, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, pictureSize %d, async %p stub!\n", iface, user, pictureSize, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    FIXME( "iface %p, async %p, bufferSize %p stub!\n", iface, async, bufferSize );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, SIZE_T *bufferUsed )
{
    FIXME( "iface %p, async %p, bufferSize %Iu, buffer %p, bufferUsed %p stub!\n", iface, async, bufferSize, buffer, bufferUsed );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetAgeGroup( IXUserImpl6 *iface, XUserHandle user, XUserAgeGroup *ageGroup )
{
    TRACE( "iface %p, user %p, ageGroup %p.\n", iface, user, ageGroup );
    if (!is_local_user( user ) || !ageGroup) return E_INVALIDARG;
    *ageGroup = XUserAgeGroup_Adult;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserCheckPrivilege( IXUserImpl6 *iface, XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege, BOOLEAN *hasPrivilege, XUserPrivilegeDenyReason *reason )
{
    TRACE( "iface %p, user %p, options %#x, privilege %d, hasPrivilege %p, reason %p.\n", iface, user, options, privilege, hasPrivilege, reason );
    if (!is_local_user( user ) || !hasPrivilege) return E_INVALIDARG;
    /* a local offline user holds every privilege; nothing is restricted here */
    *hasPrivilege = TRUE;
    if (reason) *reason = XUserPrivilegeDenyReason_None;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserResolvePrivilegeWithUiAsync( IXUserImpl6 *iface, XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, options %d, privilege %d, async %p stub!\n", iface, user, options, privilege, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolvePrivilegeWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

/* A real Xbox Live token can only come from the signed-in Microsoft account,
 * which lives outside the prefix in xodus-service; the unixlib fetches one for
 * the URL the title is about to call. When there is no signed-in account the
 * answer is E_GAMEUSER_NO_AUTH_USER -- the documented "not signed in to Xbox
 * Live" state, which a title handles by staying offline. It must not be
 * E_NOTIMPL: callers read that as an unexpected fault and retry for ever
 * (Minecraft spun on its PlayFab sign-in tens of thousands of times).
 *
 * The result is reported through the async block, since the operation itself
 * starts fine and it is the outcome that may fail. The payload is one
 * allocation laid out as the caller expects to receive it: the
 * XUserGetTokenAndSignatureData header followed by the token bytes. */
static HRESULT token_and_signature_async( XAsyncBlock *async, const char *method, const char *url,
                                          SIZE_T body_size, const void *body )
{
    struct xsts_token_params params = {0};
    XUserGetTokenAndSignatureData *data;
    char title_id[64], title_client_id[64];
    SIZE_T token_len, sig_len, size;
    char *token_copy, *sig_copy;

    if (!async) return E_INVALIDARG;

    /* Most Xbox Live endpoints carry a signature policy, and a signature covers
     * this request's own method, path and body -- so they all have to go with
     * the ask rather than being cached alongside the token. */
    params.url = url;
    params.method = method;
    params.body = body;
    params.body_size = body_size;

    /* Which title is asking. Xbox puts it in the token as a claim, and the
     * services that scope by title refuse a token claiming the wrong one --
     * loudly enough (403) to look like a broken sign-in rather than a wrong
     * claim, which is what makes this worth carrying on every request. */
    xgr_title_identity( title_id, sizeof(title_id), title_client_id, sizeof(title_client_id) );
    params.title_id = title_id;
    params.title_client_id = title_client_id;
    if (!title_id[0] || !title_client_id[0])
        WARN( "MicrosoftGame.config has no %s; the token will carry no title claim and "
              "presence and multiplayer will answer 403.\n",
              title_id[0] ? "MSAAppId" : "TitleId" );
    if (XGR_UNIX_CALL( xsts_token, &params ))
    {
        xgr_async_complete( async, E_GAMEUSER_NO_AUTH_USER, NULL );
        return S_OK;
    }

    token_len = strlen( params.token );
    sig_len = strlen( params.signature );
    size = sizeof(*data) + token_len + 1 + (sig_len ? sig_len + 1 : 0);
    if (!(data = calloc( 1, size )))
    {
        xgr_async_complete( async, E_OUTOFMEMORY, NULL );
        return S_OK;
    }

    token_copy = (char *)(data + 1);
    memcpy( token_copy, params.token, token_len + 1 );
    data->tokenSize = token_len + 1;
    data->token = token_copy;

    if (sig_len)
    {
        sig_copy = token_copy + token_len + 1;
        memcpy( sig_copy, params.signature, sig_len + 1 );
        data->signatureSize = sig_len + 1;
        data->signature = sig_copy;
    }

    TRACE( "issued a token for %s %s%s.\n", debugstr_a( method ), debugstr_a( url ),
           sig_len ? " (signed)" : "" );
    xgr_async_complete_ex( async, S_OK, data, size );
    return S_OK;
}

static HRESULT token_and_signature_result( XAsyncBlock *async, SIZE_T buffer_size, void *buffer,
                                           XUserGetTokenAndSignatureData **ptr_to_buffer, SIZE_T *buffer_used )
{
    XUserGetTokenAndSignatureData *data;
    void *payload;
    SIZE_T size;
    HRESULT hr;

    if (!async) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, &payload )) return E_PENDING;
    if (FAILED(hr)) return hr;
    if (!(data = payload)) return E_UNEXPECTED;

    size = sizeof(*data) + data->tokenSize + data->signatureSize;
    if (buffer_used) *buffer_used = size;
    if (!buffer) return S_OK;
    if (buffer_size < size) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );

    /* Copy the block and re-point its pointer at the caller's copy. */
    memcpy( buffer, data, size );
    ((XUserGetTokenAndSignatureData *)buffer)->token = (char *)buffer + sizeof(*data);
    ((XUserGetTokenAndSignatureData *)buffer)->signature =
        data->signatureSize ? (char *)buffer + sizeof(*data) + data->tokenSize : NULL;
    if (ptr_to_buffer) *ptr_to_buffer = buffer;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureAsync( IXUserImpl6 *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, const char *method, const char *url, SIZE_T headerCount, const XUserGetTokenAndSignatureHttpHeader *headers, SIZE_T bodySize, const void *bodyBuffer, XAsyncBlock *async )
{
    TRACE( "iface %p, user %p, options %d, method %s, url %s, async %p.\n",
           iface, user, options, debugstr_a( method ), debugstr_a( url ), async );
    return token_and_signature_async( async, method, url, bodySize, bodyBuffer );
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    TRACE( "iface %p, async %p, bufferSize %p.\n", iface, async, bufferSize );
    return token_and_signature_result( async, 0, NULL, NULL, bufferSize );
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, XUserGetTokenAndSignatureData **ptrToBuffer, SIZE_T *bufferUsed )
{
    TRACE( "iface %p, async %p, bufferSize %Iu, buffer %p.\n", iface, async, bufferSize, buffer );
    return token_and_signature_result( async, bufferSize, buffer, ptrToBuffer, bufferUsed );
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Async( IXUserImpl6 *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, const WCHAR *method, const WCHAR *url, SIZE_T headerCount, const XUserGetTokenAndSignatureUtf16HttpHeader *headers, SIZE_T bodySize, const void *bodyBuffer, XAsyncBlock *async )
{
    char *url_a = NULL, *method_a = NULL;
    HRESULT hr;
    int len;

    TRACE( "iface %p, user %p, options %d, method %s, url %s, async %p.\n",
           iface, user, options, debugstr_w( method ), debugstr_w( url ), async );

    if (url && (len = WideCharToMultiByte( CP_UTF8, 0, url, -1, NULL, 0, NULL, NULL )) > 0 &&
        (url_a = malloc( len )))
        WideCharToMultiByte( CP_UTF8, 0, url, -1, url_a, len, NULL, NULL );

    if (method && (len = WideCharToMultiByte( CP_UTF8, 0, method, -1, NULL, 0, NULL, NULL )) > 0 &&
        (method_a = malloc( len )))
        WideCharToMultiByte( CP_UTF8, 0, method, -1, method_a, len, NULL, NULL );

    /* The caller's method, not "GET": a signature computed over the wrong verb
     * is a signature the service rejects, and a POST signed as a GET fails in a
     * way that looks like the token being wrong. */
    hr = token_and_signature_async( async, method_a ? method_a : "GET", url_a, bodySize, bodyBuffer );
    free( method_a );
    free( url_a );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16ResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    TRACE( "iface %p, async %p, bufferSize %p.\n", iface, async, bufferSize );
    return token_and_signature_result( async, 0, NULL, NULL, bufferSize );
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Result( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, XUserGetTokenAndSignatureUtf16Data **ptrToBuffer, SIZE_T *bufferUsed )
{
    TRACE( "iface %p, async %p, bufferSize %Iu, buffer %p.\n", iface, async, bufferSize, buffer );
    return token_and_signature_result( async, bufferSize, buffer, (XUserGetTokenAndSignatureData **)ptrToBuffer, bufferUsed );
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiAsync( IXUserImpl6 *iface, XUserHandle user, const char *url, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, url %s, async %p stub!\n", iface, user, debugstr_a( url ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiUtf16Async( IXUserImpl6 *iface, XUserHandle user, const WCHAR *url, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, url %s, async %p stub!\n", iface, user, debugstr_w( url ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiUtf16Result( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserRegisterForChangeEvent( IXUserImpl6 *iface, XTaskQueueHandle queue, void *context, XUserChangeEventCallback *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );
    /* the single local user never changes; hand back a token that never fires */
    if (token) token->token = 1;
    return S_OK;
}

static BOOLEAN WINAPI x_user_XUserUnregisterForChangeEvent( IXUserImpl6 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, wait %d.\n", iface, wait );
    return TRUE;
}

static HRESULT WINAPI x_user_XUserGetSignOutDeferral( IXUserImpl6 *iface, XUserSignOutDeferralHandle *deferral )
{
    TRACE( "iface %p, deferral %p.\n", iface, deferral );
    *deferral = NULL;
    return E_GAMEUSER_DEFERRAL_NOT_AVAILABLE;
}

static void WINAPI x_user_XUserCloseSignOutDeferralHandle( IXUserImpl6 *iface, XUserSignOutDeferralHandle deferral )
{
    TRACE( "iface %p, deferral %p.\n", iface, deferral );
}

static HRESULT WINAPI x_user_XUserAddByIdWithUiAsync( IXUserImpl6 *iface, UINT64 userId, XAsyncBlock *async )
{
    FIXME( "iface %p, userId %llu, async %p stub!\n", iface, userId, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserAddByIdWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async, XUserHandle *newUser )
{
    FIXME( "iface %p, async %p, newUser %p stub!\n", iface, async, newUser );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyAsync( IXUserImpl6 *iface, XUserHandle user, XUserGetMsaTokenSilentlyOptions options, const char *scope, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, options %u, scope %s, async %p stub!\n", iface, user, options, debugstr_a( scope ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T resultTokenSize, char *resultToken, SIZE_T *resultTokenUsed )
{
    FIXME( "iface %p, async %p, resultTokenSize %Iu, resultToken %p, resultTokenUsed %p stub!\n", iface, async, resultTokenSize, resultToken, resultTokenUsed );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *tokenSize )
{
    FIXME( "iface %p, async %p, tokenSize %p stub!\n", iface, async, tokenSize );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_XUserIsStoreUser( IXUserImpl6 *iface, XUserHandle user )
{
    TRACE( "iface %p, user %p.\n", iface, user );
    return is_local_user( user );  /* the local user owns the title from the store */
}

static HRESULT WINAPI x_user_XUserPlatformRemoteConnectSetEventHandlers( IXUserImpl6 *iface, XTaskQueueHandle queue, XUserPlatformRemoteConnectEventHandlers *handlers )
{
    FIXME( "iface %p, queue %p, handlers %p stub!\n", iface, queue, handlers );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformRemoteConnectCancelPrompt( IXUserImpl6 *iface, XUserPlatformOperation operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformSpopPromptSetEventHandlers( IXUserImpl6 *iface, XTaskQueueHandle queue, XUserPlatformSpopPromptEventHandler *handler, void *context )
{
    FIXME( "iface %p, queue %p, handler %p, context %p stub!\n", iface, queue, handler, context );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformSpopPromptComplete( IXUserImpl6 *iface, XUserPlatformOperation operation, XUserPlatformOperationResult result )
{
    FIXME( "iface %p, operation %p, result %d stub!\n", iface, operation, result );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_XUserIsSignOutPresent( IXUserImpl6 *iface )
{
    TRACE( "iface %p.\n", iface );
    return FALSE;
}

static HRESULT WINAPI x_user_XUserSignOutAsync( IXUserImpl6 *iface, XUserHandle user, XAsyncBlock *async )
{
    TRACE( "iface %p, user %p, async %p.\n", iface, user, async );
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_user_XUserSignOutResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    HRESULT hr;
    TRACE( "iface %p, async %p.\n", iface, async );
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    return hr;
}

static const struct IXUserImpl6Vtbl x_user_vtbl =
{
    x_user_QueryInterface,
    x_user_AddRef,
    x_user_Release,
    /* IXUserImpl methods */
    x_user_XUserDuplicateHandle,
    x_user_XUserCloseHandle,
    x_user_XUserCompare,
    x_user_XUserGetMaxUsers,
    x_user_XUserAddAsync,
    x_user_XUserAddResult,
    x_user_XUserGetLocalId,
    x_user_XUserFindUserByLocalId,
    x_user_XUserGetId,
    x_user_XUserFindUserById,
    x_user_XUserGetIsGuest,
    x_user_XUserGetState,
    __PADDING__,
    x_user_XUserGetGamerPictureAsync,
    x_user_XUserGetGamerPictureResultSize,
    x_user_XUserGetGamerPictureResult,
    x_user_XUserGetAgeGroup,
    x_user_XUserCheckPrivilege,
    x_user_XUserResolvePrivilegeWithUiAsync,
    x_user_XUserResolvePrivilegeWithUiResult,
    x_user_XUserGetTokenAndSignatureAsync,
    x_user_XUserGetTokenAndSignatureResultSize,
    x_user_XUserGetTokenAndSignatureResult,
    x_user_XUserGetTokenAndSignatureUtf16Async,
    x_user_XUserGetTokenAndSignatureUtf16ResultSize,
    x_user_XUserGetTokenAndSignatureUtf16Result,
    x_user_XUserResolveIssueWithUiAsync,
    x_user_XUserResolveIssueWithUiResult,
    x_user_XUserResolveIssueWithUiUtf16Async,
    x_user_XUserResolveIssueWithUiUtf16Result,
    x_user_XUserRegisterForChangeEvent,
    x_user_XUserUnregisterForChangeEvent,
    x_user_XUserGetSignOutDeferral,
    x_user_XUserCloseSignOutDeferralHandle,
    /* IXUserImpl2 methods */
    x_user_XUserAddByIdWithUiAsync,
    x_user_XUserAddByIdWithUiResult,
    /* IXUserImpl3 methods */
    x_user_XUserGetMsaTokenSilentlyAsync,
    x_user_XUserGetMsaTokenSilentlyResult,
    x_user_XUserGetMsaTokenSilentlyResultSize,
    /* IXUserImpl4 methods */
    x_user_XUserIsStoreUser,
    /* IXUserImpl5 methods */
    x_user_XUserPlatformRemoteConnectSetEventHandlers,
    x_user_XUserPlatformRemoteConnectCancelPrompt,
    x_user_XUserPlatformSpopPromptSetEventHandlers,
    x_user_XUserPlatformSpopPromptComplete,
    /* IXUserImpl6 methods */
    x_user_XUserIsSignOutPresent,
    x_user_XUserSignOutAsync,
    x_user_XUserSignOutResult,
};

static inline struct x_user *impl_from_IXUserGamertagImpl( IXUserGamertagImpl *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserGamertagImpl_iface );
}

static HRESULT WINAPI x_user_gamertag_QueryInterface( IXUserGamertagImpl *iface, REFIID riid, void **out )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_QueryInterface( &impl->IXUserImpl6_iface, riid, out );
}

static ULONG WINAPI x_user_gamertag_AddRef( IXUserGamertagImpl *iface )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_AddRef( &impl->IXUserImpl6_iface );
}

static ULONG WINAPI x_user_gamertag_Release( IXUserGamertagImpl *iface )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_Release( &impl->IXUserImpl6_iface );
}

static HRESULT WINAPI x_user_gamertag_XUserGetGamertag( IXUserGamertagImpl *iface, XUserHandle user, XUserGamertagComponent gamertagComponent, SIZE_T gamertagSize, char *gamertag, SIZE_T *gamertagUsed )
{
    static const char classic[] = "Player", modern[] = "Player", suffix[] = "", unique[] = "Player#0000";
    const char *value;
    SIZE_T len;

    TRACE( "iface %p, user %p, gamertagComponent %d, gamertagSize %Iu, gamertag %p.\n", iface, user, gamertagComponent, gamertagSize, gamertag );
    if (!gamertag) return E_INVALIDARG;

    /* Report the signed-in account's own gamertag when there is one. A classic
     * gamertag has no modern suffix, so every component but the suffix is just
     * the tag itself. */
    ensure_live_identity();
    if (live_identity.gamertag[0])
    {
        value = (gamertagComponent == XUserGamertagComponent_ModernSuffix) ? "" : live_identity.gamertag;
        len = strlen( value ) + 1;
        if (gamertagSize < len) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
        memcpy( gamertag, value, len );
        if (gamertagUsed) *gamertagUsed = len;
        return S_OK;
    }

    switch (gamertagComponent)
    {
    case XUserGamertagComponent_ModernSuffix: value = suffix; break;
    case XUserGamertagComponent_UniqueModern: value = unique; break;
    case XUserGamertagComponent_Modern:       value = modern; break;
    default:                                  value = classic; break;
    }
    len = strlen( value ) + 1;
    if (gamertagSize < len) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    memcpy( gamertag, value, len );
    if (gamertagUsed) *gamertagUsed = len;
    return S_OK;
}

static const struct IXUserGamertagImplVtbl x_user_gamertag_vtbl =
{
    x_user_gamertag_QueryInterface,
    x_user_gamertag_AddRef,
    x_user_gamertag_Release,
    /* IXUserGamertag methods */
    x_user_gamertag_XUserGetGamertag,
};

static inline struct x_user *impl_from_IXUserDeviceImpl2( IXUserDeviceImpl2 *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserDeviceImpl2_iface );
}

static HRESULT WINAPI x_user_device_QueryInterface( IXUserDeviceImpl2 *iface, REFIID iid, void **out )
{
    struct x_user *impl = impl_from_IXUserDeviceImpl2( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown          ) ||
        IsEqualGUID( iid, &IID_IXUserDeviceImpl  ) ||
        IsEqualGUID( iid, &IID_IXUserDeviceImpl2 ))
    {
        IXUserDeviceImpl2_AddRef( *out = &impl->IXUserDeviceImpl2_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_user_device_AddRef( IXUserDeviceImpl2 *iface )
{
    struct x_user *impl = impl_from_IXUserDeviceImpl2( iface );
    return IXUserImpl6_AddRef( &impl->IXUserImpl6_iface );
}

static ULONG WINAPI x_user_device_Release( IXUserDeviceImpl2 *iface )
{
    struct x_user *impl = impl_from_IXUserDeviceImpl2( iface );
    return IXUserImpl6_Release( &impl->IXUserImpl6_iface );
}

static HRESULT WINAPI x_user_device_XUserFindForDevice( IXUserDeviceImpl2 *iface, const APP_LOCAL_DEVICE_ID *deviceId, XUserHandle *handle )
{
    FIXME( "iface %p, deviceId %p, handle %p stub!\n", iface, deviceId, handle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserRegisterForDeviceAssociationChanged( IXUserDeviceImpl2 *iface, XTaskQueueHandle queue, void *context, XUserDeviceAssociationChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p stub!\n", iface, queue, context, callback, token );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_device_XUserUnregisterForDeviceAssociationChanged( IXUserDeviceImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, token %p, wait %d stub!\n", iface, &token, wait );
    return FALSE;
}

static HRESULT WINAPI x_user_device_XUserGetDefaultAudioEndpointUtf16( IXUserDeviceImpl2 *iface, XUserLocalId user, XUserDefaultAudioEndpointKind defaultAudioEndpointKind, SIZE_T endpointIdUtf16Count, WCHAR *endpointIdUtf16, SIZE_T *endpointIdUtf16Used )
{
    FIXME( "iface %p, user %p, defaultAudioEndpointKind %d, endpointIdUtf16Count %Iu, endpointIdUtf16 %p, endpointIdUtf16Used %p stub!\n", iface, &user, defaultAudioEndpointKind, endpointIdUtf16Count, endpointIdUtf16, endpointIdUtf16Used );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserRegisterForDefaultAudioEndpointUtf16Changed( IXUserDeviceImpl2 *iface, XTaskQueueHandle queue, void *context, XUserDefaultAudioEndpointUtf16ChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p stub!\n", iface, queue, context, callback, token );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_device_XUserUnregisterForDefaultAudioEndpointUtf16Changed( IXUserDeviceImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, token %p, wait %d stub!\n", iface, &token, wait );
    return FALSE;
}

static HRESULT WINAPI x_user_device_XUserFindControllerForUserWithUiAsync( IXUserDeviceImpl2 *iface, XUserHandle user, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, async %p stub!\n", iface, user, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserFindControllerForUserWithUiResult( IXUserDeviceImpl2 *iface, XAsyncBlock *async, APP_LOCAL_DEVICE_ID *deviceId )
{
    FIXME( "iface %p, async %p, deviceId %p stub!\n", iface, async, deviceId );
    return E_NOTIMPL;
}

static const struct IXUserDeviceImpl2Vtbl x_user_device_vtbl =
{
    x_user_device_QueryInterface,
    x_user_device_AddRef,
    x_user_device_Release,
    /* IXUserDeviceImpl/IXUserDeviceImpl2 methods */
    x_user_device_XUserFindForDevice,
    x_user_device_XUserRegisterForDeviceAssociationChanged,
    x_user_device_XUserUnregisterForDeviceAssociationChanged,
    x_user_device_XUserGetDefaultAudioEndpointUtf16,
    x_user_device_XUserRegisterForDefaultAudioEndpointUtf16Changed,
    x_user_device_XUserUnregisterForDefaultAudioEndpointUtf16Changed,
    x_user_device_XUserFindControllerForUserWithUiAsync,
    x_user_device_XUserFindControllerForUserWithUiResult,
};

static struct x_user x_user =
{
    {&x_user_vtbl},
    {&x_user_gamertag_vtbl},
    {&x_user_device_vtbl},
    0,
};

IXUserImpl *x_user_impl = (IXUserImpl *)&x_user.IXUserImpl6_iface;
IXUserDeviceImpl *x_user_device_impl = (IXUserDeviceImpl *)&x_user.IXUserDeviceImpl2_iface;
