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

#include "ntstatus.h"
#define WIN32_NO_STATUS
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

/* Keep the result bytes separate from the object the handle points to.
 * XAsyncGetResult copies those bytes, while XUserAddResult returns the handle. */
static XUserHandle local_user_result = (XUserHandle)&local_user;

static XUserHandle local_user_handle( void )
{
    return (XUserHandle)&local_user;
}

static BOOL is_local_user( XUserHandle user )
{
    return user == local_user_handle();
}

static BOOL account_roles_enabled(void)
{
    char enabled[8];
    return GetEnvironmentVariableA("XGR_ACCOUNT_ROLES_V1",enabled,sizeof(enabled)) && !strcmp(enabled,"1");
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
 * and shows nobody as signed in. Only latched once it succeeds: a single
 * transient failure used to leave the process offline for the rest of its life,
 * which showed up as being signed out until the title was restarted.
 *
 * The request runs on the thread pool, never on the thread asking. XUserGetId
 * and XUserGetGamertag are calls a title makes from its main thread and expects
 * back at once; doing the service round trip inline cost half a second per
 * call with the service down, and the whole frame with it stalled. So the
 * resolution starts at XUserAddAsync, the very first question waits a bounded
 * time for it -- keeping the XUID a title reads at startup the real one in the
 * ordinary case -- and every later question takes what is known. A failure is
 * remembered for a while, so a title polling the getters does not turn into a
 * stream of requests at a service that is not there. */
#define IDENTITY_FIRST_WAIT_MS 15000
#define IDENTITY_RETRY_MS      10000

static struct
{
    UINT64 xuid;
    char gamertag[XGR_IDENTITY_MAX];
    BOOL resolved, resolving, attempted, waited;
    ULONGLONG retry_at;
} live_identity;

static SRWLOCK live_identity_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE live_identity_done = CONDITION_VARIABLE_INIT;

/* Keep this module mapped while a thread of ours may be running in it. */
static void pin_module( void )
{
    HMODULE module;
    GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                        (const WCHAR *)pin_module, &module );
}

static void CALLBACK resolve_live_identity( TP_CALLBACK_INSTANCE *instance, void *context )
{
    struct xsts_token_params *params;
    UINT64 xuid = 0;
    NTSTATUS status = STATUS_NO_MEMORY;
    const char *p;

    /* Any Xbox Live relying party returns the display claims; this one is the
     * canonical choice and is what the account's own services use. */
    if ((params = calloc( 1, sizeof(*params) )))
    {
        params->url = "https://xboxlive.com";
        status = XGR_UNIX_CALL( xsts_token, params );
    }

    AcquireSRWLockExclusive( &live_identity_lock );
    if (!status)
    {
        for (p = params->xuid; *p >= '0' && *p <= '9'; p++)
            xuid = xuid * 10 + (*p - '0');
        live_identity.xuid = xuid;
        memcpy( live_identity.gamertag, params->gamertag, sizeof(live_identity.gamertag) );
        live_identity.gamertag[sizeof(live_identity.gamertag) - 1] = 0;
        live_identity.resolved = TRUE;
        TRACE( "signed in as %s (xuid %I64u).\n", debugstr_a( live_identity.gamertag ), live_identity.xuid );
    }
    else
    {
        live_identity.retry_at = GetTickCount64() + IDENTITY_RETRY_MS;
        WARN( "no Xbox Live identity yet (%#lx); will try again in %u s.\n", status, IDENTITY_RETRY_MS / 1000 );
    }
    live_identity.resolving = FALSE;
    live_identity.attempted = TRUE;
    ReleaseSRWLockExclusive( &live_identity_lock );
    WakeAllConditionVariable( &live_identity_done );

    if (params)
    {
        SecureZeroMemory( params->token, sizeof(params->token) );
        SecureZeroMemory( params->signature, sizeof(params->signature) );
        free( params );
    }
}

/* Caller holds live_identity_lock exclusively. */
static void start_live_identity_locked( void )
{
    if (live_identity.resolved || live_identity.resolving || GetTickCount64() < live_identity.retry_at) return;
    pin_module();
    live_identity.resolving = TRUE;
    if (!TrySubmitThreadpoolCallback( resolve_live_identity, NULL, NULL ))
    {
        live_identity.resolving = FALSE;
        live_identity.retry_at = GetTickCount64() + IDENTITY_RETRY_MS;
    }
}

static void start_live_identity( void )
{
    AcquireSRWLockExclusive( &live_identity_lock );
    start_live_identity_locked();
    ReleaseSRWLockExclusive( &live_identity_lock );
}

/* What is known about the signed-in identity right now. */
static BOOL get_live_identity( UINT64 *xuid, char *gamertag, SIZE_T gamertag_size )
{
    ULONGLONG deadline, now;
    BOOL resolved;

    AcquireSRWLockExclusive( &live_identity_lock );
    start_live_identity_locked();
    if (!live_identity.attempted && !live_identity.waited)
    {
        deadline = GetTickCount64() + IDENTITY_FIRST_WAIT_MS;
        while (live_identity.resolving && (now = GetTickCount64()) < deadline)
            SleepConditionVariableSRW( &live_identity_done, &live_identity_lock, deadline - now, 0 );
        /* Whatever happened, nobody else waits: a stalled service costs the
         * first question its bounded wait and no more. */
        live_identity.waited = TRUE;
    }
    resolved = live_identity.resolved;
    if (xuid) *xuid = live_identity.xuid;
    if (gamertag) lstrcpynA( gamertag, live_identity.gamertag, gamertag_size );
    ReleaseSRWLockExclusive( &live_identity_lock );
    return resolved;
}

/* Account-roles state (XGR_ACCOUNT_ROLES_V1).
 *
 * Who is signed in to the Store and Player roles, and whether the Store account
 * holds the running game, live in the service. Asking it is a socket round
 * trip -- and for the entitlement, about once a minute, a licensing request of
 * up to 25 s. None of that may happen on a title's thread or queue:
 * XUserGetState, XUserIsStoreUser and XStoreIsLicenseValid are cheap local
 * checks on Windows that titles call every frame, and the change notifications
 * used to poll on the title's own queue, holding up everything queued behind
 * them (in strict manual mode, the title's dispatch thread itself).
 *
 * So one private thread per source keeps the last definitive answer here, and
 * the title-facing calls read it. A failed exchange -- the service restarting,
 * a timeout -- is not an answer: it leaves the last state alone, where it used
 * to read as a sign-out or a lost licence, and a licence handle never recovered
 * from it. An answer in a protocol version this side cannot read is
 * definitive, and fails closed. A source is polled while something watches it
 * or a title asked about it in the last half minute.
 *
 * Outside that window nothing polls, so what is kept can be as old as the
 * last question. A title checking its licence once a minute with nothing
 * registered would then be told about a Store sign-out a whole question late,
 * and a sign-in likewise. So the first question after a quiet spell -- when
 * nothing has asked the service for ROLE_FRESH_MS -- wakes the poller and
 * waits for its answer, for at most ROLE_STALE_WAIT_MS; a service that does
 * not answer in time gets the last state, as during any outage. Freshness
 * bound, then: while polled, about ROLE_POLL_MS plus one exchange; after a
 * quiet spell, current unless the service took longer than the wait. */
enum role_source { ROLE_SOURCE_ACCOUNTS, ROLE_SOURCE_ENTITLEMENT, ROLE_SOURCE_COUNT };
#define ROLE_POLL_MS       1000
#define ROLE_DEMAND_MS     30000
#define ROLE_FRESH_MS      3000
#define ROLE_STALE_WAIT_MS 1000

/* What a caller of role_state_get wants. */
#define ROLE_GET_SEED  0x1  /* ask synchronously if there has never been an answer */
#define ROLE_GET_FRESH 0x2  /* after a quiet spell, wait (bounded) for a current answer */

static struct role_source_state
{
    BOOL known, seeded, started;
    struct account_roles_params roles;  /* ROLE_SOURCE_ACCOUNTS */
    UINT32 active;                      /* ROLE_SOURCE_ENTITLEMENT */
    ULONGLONG wanted_until;
    ULONGLONG checked_at;               /* when the last exchange ended, whatever it said */
    LONG generation;                    /* counts those exchanges */
    LONG watchers;
    HANDLE wake;
} role_state[ROLE_SOURCE_COUNT];
static SRWLOCK role_state_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE role_state_checked = CONDITION_VARIABLE_INIT;

static void role_watches_update( enum role_source source );

/* Keep what an exchange said, if it said anything. */
static BOOL role_state_store( enum role_source source, NTSTATUS status,
                              const struct account_roles_params *roles, UINT32 active )
{
    struct role_source_state *state = &role_state[source];

    if (status && status != STATUS_NOT_SUPPORTED)
    {
        TRACE( "role source %u unavailable (%#lx); keeping the last state.\n", source, status );
        return FALSE;
    }
    AcquireSRWLockExclusive( &role_state_lock );
    state->known = TRUE;
    memset( &state->roles, 0, sizeof(state->roles) );
    state->active = 0;
    if (!status && source == ROLE_SOURCE_ACCOUNTS) state->roles = *roles;
    else if (!status) state->active = active;
    ReleaseSRWLockExclusive( &role_state_lock );
    return TRUE;
}

/* An exchange with the service has ended, answered or not: whoever waits for a
 * current answer can stop waiting. */
static void role_state_note_checked( enum role_source source )
{
    AcquireSRWLockExclusive( &role_state_lock );
    role_state[source].checked_at = GetTickCount64();
    role_state[source].generation++;
    ReleaseSRWLockExclusive( &role_state_lock );
    WakeAllConditionVariable( &role_state_checked );
}

static NTSTATUS role_state_fetch( enum role_source source )
{
    struct store_entitlement_params entitlement = {{0}};
    struct account_roles_params roles = {0};
    NTSTATUS status;

    if (source == ROLE_SOURCE_ACCOUNTS) status = XGR_UNIX_CALL( account_roles, &roles );
    else if (!xgr_config_element( "StoreId", entitlement.store_id, sizeof(entitlement.store_id) ))
        return STATUS_NOT_FOUND;
    else status = XGR_UNIX_CALL( store_entitlement, &entitlement );
    role_state_store( source, status, &roles, entitlement.active );
    role_state_note_checked( source );
    return status;
}

static BOOL role_state_peek( enum role_source source, struct account_roles_params *roles, UINT32 *active )
{
    BOOL known;

    AcquireSRWLockShared( &role_state_lock );
    known = role_state[source].known;
    if (roles) *roles = role_state[source].roles;
    if (active) *active = role_state[source].active;
    ReleaseSRWLockShared( &role_state_lock );
    return known;
}

static DWORD WINAPI role_poller( void *arg )
{
    enum role_source source = (ULONG_PTR)arg;
    struct role_source_state *state = &role_state[source];
    BOOL demand;

    for (;;)
    {
        AcquireSRWLockShared( &role_state_lock );
        demand = state->watchers > 0 || GetTickCount64() < state->wanted_until;
        ReleaseSRWLockShared( &role_state_lock );
        if (demand)
        {
            role_state_fetch( source );
            role_watches_update( source );
        }
        WaitForSingleObject( state->wake, demand ? ROLE_POLL_MS : INFINITE );
    }
    return 0;
}

/* Note that a title cares about a source -- `watchers` adds or drops a
 * standing interest -- and make sure something is polling it. Returns TRUE
 * when what is kept may be stale: nothing was polling it, and nothing has
 * asked the service for a while. `generation` then says which exchange a
 * fresher answer has to come after. */
static BOOL role_source_want( enum role_source source, LONG watchers, LONG *generation )
{
    struct role_source_state *state = &role_state[source];
    ULONGLONG now = GetTickCount64();
    BOOL idle, stale, start = FALSE;
    HANDLE thread;

    AcquireSRWLockExclusive( &role_state_lock );
    idle = state->watchers <= 0 && now >= state->wanted_until;
    stale = idle && now - state->checked_at >= ROLE_FRESH_MS;
    if (generation) *generation = state->generation;
    state->watchers += watchers;
    if (watchers >= 0) state->wanted_until = now + ROLE_DEMAND_MS;
    if (!state->started && (state->wake = CreateEventW( NULL, FALSE, FALSE, NULL )))
        start = state->started = TRUE;
    ReleaseSRWLockExclusive( &role_state_lock );

    if (start)
    {
        pin_module();
        if ((thread = CreateThread( NULL, 0, role_poller, (void *)(ULONG_PTR)source, 0, NULL )))
            CloseHandle( thread );
        else ERR( "could not start the role poller: %lu.\n", GetLastError() );
    }
    else if (idle && watchers >= 0 && state->wake) SetEvent( state->wake );
    return stale && watchers >= 0 && state->wake;
}

/* The last definitive answer, if there has been one. With ROLE_GET_SEED, a
 * source that has never answered is asked once, synchronously and bounded, so
 * the first question of a process does not have to wait for a poll. With
 * ROLE_GET_FRESH, the first question after a quiet spell waits a bounded time
 * for the poller's next answer rather than reporting one from before it. */
static BOOL role_state_get( enum role_source source, DWORD flags, struct account_roles_params *roles, UINT32 *active )
{
    struct role_source_state *state = &role_state[source];
    ULONGLONG deadline, now;
    LONG generation;
    BOOL ask, stale;

    stale = role_source_want( source, 0, &generation );
    if (role_state_peek( source, roles, active ))
    {
        if (!stale || !(flags & ROLE_GET_FRESH)) return TRUE;
        AcquireSRWLockExclusive( &role_state_lock );
        deadline = GetTickCount64() + ROLE_STALE_WAIT_MS;
        while (state->generation == generation && (now = GetTickCount64()) < deadline)
            SleepConditionVariableSRW( &role_state_checked, &role_state_lock, deadline - now, 0 );
        if (state->generation == generation)
            TRACE( "role source %u did not answer within %u ms; reporting the last state.\n",
                   source, ROLE_STALE_WAIT_MS );
        ReleaseSRWLockExclusive( &role_state_lock );
        return role_state_peek( source, roles, active );
    }
    if (!(flags & ROLE_GET_SEED)) return FALSE;
    AcquireSRWLockExclusive( &role_state_lock );
    ask = !state->seeded;
    state->seeded = TRUE;
    ReleaseSRWLockExclusive( &role_state_lock );
    if (ask) role_state_fetch( source );
    return role_state_peek( source, roles, active );
}

BOOL xgr_role_entitlement( UINT32 *active, BOOL fresh )
{
    return role_state_get( ROLE_SOURCE_ENTITLEMENT, fresh ? ROLE_GET_FRESH : 0, NULL, active );
}

void xgr_role_entitlement_store( NTSTATUS status, UINT32 active )
{
    role_state_store( ROLE_SOURCE_ENTITLEMENT, status, NULL, active );
    role_state_note_checked( ROLE_SOURCE_ENTITLEMENT );
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
    struct account_roles_params roles;
    TRACE( "iface %p, options %d, async %p.\n", iface, options, async );

    if (!async) return E_INVALIDARG;
    if (account_roles_enabled())
    {
        /* Asked afresh, since this is where a title learns whether anyone is
         * signed in at all (bounded by the unix call's own deadline); a
         * failed exchange falls back to the last answer there was. */
        role_state_fetch( ROLE_SOURCE_ACCOUNTS );
        if (!role_state_get( ROLE_SOURCE_ACCOUNTS, 0, &roles, NULL ) || !roles.player_signed_in)
            return HRESULT_FROM_WIN32(ERROR_NO_SUCH_LOGON_SESSION);
    }
    /* The identity is wanted next; get it going while the title carries on. */
    start_live_identity();

    /* Nothing to negotiate for a local user, so complete before returning. */
    TRACE( "user handle async result: %Iu bytes.\n", sizeof(local_user_result) );
    xgr_async_complete_ex( async, S_OK, &local_user_result, sizeof(local_user_result) );
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
    *newUser = *(XUserHandle *)payload;
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
    UINT64 xuid = 0;

    TRACE( "iface %p, user %p, userId %p.\n", iface, user, userId );

    if (!is_local_user( user ) || !userId) return E_INVALIDARG;

    /* Prefer the XUID the Xbox Live token was issued to; XGR_XUID (the one the
     * user's Windows install saved under) remains the offline fallback so
     * on-disk save layouts still line up. */
    if (!get_live_identity( &xuid, NULL, 0 ) || !xuid) xuid = local_user.id ? local_user.id : xgr_local_xuid();
    *userId = xuid;
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
    struct account_roles_params roles;
    TRACE( "iface %p, user %p, state %p.\n", iface, user, state );

    if (!is_local_user( user ) || !state) return E_INVALIDARG;
    /* The handle was issued signed in; only a definitive answer changes that. */
    *state = XUserState_SignedIn;
    if (account_roles_enabled() &&
        role_state_get( ROLE_SOURCE_ACCOUNTS, ROLE_GET_SEED | ROLE_GET_FRESH, &roles, NULL ) &&
        !roles.player_signed_in)
        *state = XUserState_SignedOut;
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
 * The request is an XAsyncBegin operation. The *Async call copies what it
 * needs and returns, and the operation's DoWork is scheduled on the caller's
 * queue like any other async work. DoWork itself only hands the round trip to
 * a thread of ours and returns: the exchange can take up to 65 s with its
 * retries, and in strict manual mode (XGR_STRICT_MANUAL_QUEUES, Halo Infinite)
 * DoWork runs on the title's own Work port, so doing it there held the
 * title's dispatch thread -- and everything queued behind it -- for that long
 * when the service stalled. That thread completes the operation, and nothing
 * else does, so the request stays alive until it has; it is still the queue's
 * work, so terminating the queue waits for it as before. Cancel is not
 * honoured, as before: the exchange is bounded and finishes on its own.
 *
 * The provider owns the result until the caller takes it, and Cleanup
 * releases it. It used to be handed to the block and never freed -- a
 * few KB, bearer token included, left behind for every Xbox Live call a title
 * made. As with every provider-driven operation, taking the result consumes
 * it. The narrow and UTF-16 entry points lay the result out differently, so
 * each has its own identity and the size reported is the one its Result
 * function will write. */
struct token_request
{
    struct xsts_token_params params;
    XAsyncBlock *async;                /* set when DoWork hands the request off */
    char *url, *method;
    void *body;
    BOOL wide;
    char title_id[64], title_client_id[64];
    SIZE_T token_len, signature_len;   /* UTF-8, without terminators */
    int token_count, signature_count;  /* UTF-16, with terminators */
};

static const char token_identity, token_utf16_identity;

static void token_request_free( struct token_request *req )
{
    SecureZeroMemory( req->params.token, sizeof(req->params.token) );
    SecureZeroMemory( req->params.signature, sizeof(req->params.signature) );
    free( req->url );
    free( req->method );
    free( req->body );
    free( req );
}

static SIZE_T token_result_size( const struct token_request *req )
{
    if (req->wide)
        return sizeof(XUserGetTokenAndSignatureUtf16Data) + (req->token_count + req->signature_count) * sizeof(WCHAR);
    return sizeof(XUserGetTokenAndSignatureData) + req->token_len + 1 + (req->signature_len ? req->signature_len + 1 : 0);
}

static void token_fill_result( const struct token_request *req, void *buffer )
{
    if (req->wide)
    {
        XUserGetTokenAndSignatureUtf16Data *wide = buffer;
        WCHAR *token = (WCHAR *)(wide + 1), *signature = req->signature_count ? token + req->token_count : NULL;

        MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, req->params.token, -1, token, req->token_count );
        if (signature)
            MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, req->params.signature, -1, signature, req->signature_count );
        wide->tokenCount = req->token_count * sizeof(WCHAR);
        wide->signatureCount = req->signature_count * sizeof(WCHAR);
        wide->token = token;
        wide->signature = signature;
    }
    else
    {
        XUserGetTokenAndSignatureData *data = buffer;
        char *token = (char *)(data + 1), *signature = token + req->token_len + 1;

        memcpy( token, req->params.token, req->token_len + 1 );
        data->tokenSize = req->token_len + 1;
        data->token = token;
        data->signatureSize = 0;
        data->signature = NULL;
        if (req->signature_len)
        {
            memcpy( signature, req->params.signature, req->signature_len + 1 );
            data->signatureSize = req->signature_len + 1;
            data->signature = signature;
        }
    }
}

/* The round trip itself, off the title's threads. */
static void token_request_run( struct token_request *req )
{
    HRESULT hr = S_OK;

    if (XGR_UNIX_CALL( xsts_token, &req->params )) hr = E_GAMEUSER_NO_AUTH_USER;
    else
    {
        req->token_len = strlen( req->params.token );
        req->signature_len = strlen( req->params.signature );
        if (req->wide &&
            (!(req->token_count = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, req->params.token, -1, NULL, 0 )) ||
             (req->signature_len &&
              !(req->signature_count = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, req->params.signature, -1, NULL, 0 )))))
            hr = HRESULT_FROM_WIN32( GetLastError() );
        else
            TRACE( "issued a token for %s %s%s.\n", debugstr_a( req->method ), debugstr_a( req->url ),
                   req->signature_len ? " (signed)" : "" );
    }
    /* Completion may release the request on another thread: the last use. */
    IXThreadingImpl_XAsyncComplete( x_threading_impl, req->async, hr, SUCCEEDED(hr) ? token_result_size( req ) : 0 );
}

static void CALLBACK token_request_detached( TP_CALLBACK_INSTANCE *instance, void *context )
{
    token_request_run( context );
}

static HRESULT CALLBACK token_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct token_request *req = data->context;
    HRESULT hr;

    switch (op)
    {
    case XAsyncOp_Begin:
        return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );
    case XAsyncOp_DoWork:
        req->async = data->async;
        pin_module();
        if (FAILED(hr = xgr_async_run_detached( data->async, token_request_detached, req )))
        {
            WARN( "no thread for the token request (%#lx); asking on this one.\n", hr );
            token_request_run( req );
        }
        return E_PENDING;
    case XAsyncOp_GetResult:
        token_fill_result( req, data->buffer );
        return S_OK;
    case XAsyncOp_Cleanup:
        token_request_free( req );
        return S_OK;
    default:
        return S_OK;
    }
}

static HRESULT token_and_signature_async( XAsyncBlock *async, XUserGetTokenAndSignatureOptions options,
                                          const char *method, const char *url,
                                          SIZE_T body_size, const void *body, BOOL wide )
{
    struct token_request *req;
    HRESULT hr;

    if (!async) return E_INVALIDARG;
    if (!(req = calloc( 1, sizeof(*req) ))) return E_OUTOFMEMORY;
    req->wide = wide;

    /* Most Xbox Live endpoints carry a signature policy, and a signature covers
     * this request's own method, path and body -- so they all have to go with
     * the ask rather than being cached alongside the token. The caller's
     * buffers are only promised for the length of this call, so they are
     * copied; a body over the service's limit is not, and the unixlib refuses
     * it by its size. */
    if (!body) body_size = 0;
    if (!(req->url = strdup( url ? url : "" )) || !(req->method = strdup( method ? method : "GET" )) ||
        (body_size && body_size <= 6 * 1024 * 1024 && !(req->body = malloc( body_size ))))
    {
        token_request_free( req );
        return E_OUTOFMEMORY;
    }
    if (req->body) memcpy( req->body, body, body_size );
    req->params.url = req->url;
    req->params.method = req->method;
    req->params.body = req->body;
    req->params.body_size = body_size;
    /* A title asks for a fresh token after a service refused the last one;
     * answering from the service's cache would hand back the refused token. */
    if (options & XUserGetTokenAndSignatureOptions_ForceRefresh) req->params.flags |= XGR_XSTS_FORCE_REFRESH;

    /* Which title is asking. Xbox puts it in the token as a claim, and the
     * services that scope by title refuse a token claiming the wrong one --
     * loudly enough (403) to look like a broken sign-in rather than a wrong
     * claim, which is what makes this worth carrying on every request. */
    xgr_title_identity( req->title_id, sizeof(req->title_id), req->title_client_id, sizeof(req->title_client_id) );
    req->params.title_id = req->title_id;
    req->params.title_client_id = req->title_client_id;
    if (!req->title_id[0])
        WARN( "MicrosoftGame.config has no TitleId; the token request cannot identify its title.\n" );
    else if (!req->title_client_id[0])
        TRACE( "requesting Windows title authentication for %s (no MSAAppId).\n", req->title_id );

    hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, req, wide ? &token_utf16_identity : &token_identity,
                                      "XUserGetTokenAndSignature", token_provider );
    if (FAILED(hr)) token_request_free( req );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureAsync( IXUserImpl6 *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, const char *method, const char *url, SIZE_T headerCount, const XUserGetTokenAndSignatureHttpHeader *headers, SIZE_T bodySize, const void *bodyBuffer, XAsyncBlock *async )
{
    TRACE( "iface %p, user %p, options %d, method %s, url %s, async %p.\n",
           iface, user, options, debugstr_a( method ), debugstr_a( url ), async );
    return token_and_signature_async( async, options, method, url, bodySize, bodyBuffer, FALSE );
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    TRACE( "iface %p, async %p, bufferSize %p.\n", iface, async, bufferSize );
    return IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, bufferSize );
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, XUserGetTokenAndSignatureData **ptrToBuffer, SIZE_T *bufferUsed )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, bufferSize %Iu, buffer %p.\n", iface, async, bufferSize, buffer );
    if (!buffer && bufferSize) return E_INVALIDARG;
    hr = IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, &token_identity, bufferSize, buffer, bufferUsed );
    if (SUCCEEDED(hr) && ptrToBuffer) *ptrToBuffer = buffer;
    return hr;
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
    hr = token_and_signature_async( async, options, method_a ? method_a : "GET", url_a, bodySize, bodyBuffer, TRUE );
    free( method_a );
    free( url_a );
    return hr;
}

/* Both the result buffer size and the documented token/signature counts are
 * bytes; the strings are converted from the service's UTF-8 when DoWork
 * finishes, so the size reported is exact. */
static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16ResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    TRACE( "iface %p, async %p, bufferSize %p.\n", iface, async, bufferSize );
    if (!bufferSize) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, bufferSize );
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Result( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, XUserGetTokenAndSignatureUtf16Data **ptrToBuffer, SIZE_T *bufferUsed )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, bufferSize %Iu, buffer %p.\n", iface, async, bufferSize, buffer );
    if (!buffer && bufferSize) return E_INVALIDARG;
    hr = IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, &token_utf16_identity, bufferSize, buffer, bufferUsed );
    if (SUCCEEDED(hr) && ptrToBuffer) *ptrToBuffer = buffer;
    return hr;
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

/* Profile-mode change notifications. The role pollers above decide when the
 * state changed; a watch only remembers what it last told the title and
 * delivers the news on the caller's Completion port -- nothing is ever run on
 * the caller's Work port. Registration owns one ref, each queued notification
 * and each poller pass another. Unregister stops delivery even if a slow
 * Store request is still finishing. */
struct role_watch
{
    struct role_watch *next;
    LONG refs;
    UINT64 token;
    enum xgr_role_watch_kind kind;
    void *owner, *context;
    XTaskQueueHandle queue;
    XUserChangeEventCallback *user_callback;
    XStoreGameLicenseChangedCallback *store_callback;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE idle;
    BOOL stopped, state, known;
    DWORD callback_thread;
};
struct role_notification { struct role_watch *watch; BOOL state; };
static SRWLOCK role_watches_lock = SRWLOCK_INIT;
static struct role_watch *role_watches;
static UINT64 role_watch_next_token = 0x10000;

static enum role_source role_watch_source( enum xgr_role_watch_kind kind )
{
    return kind == XGR_ROLE_WATCH_USER ? ROLE_SOURCE_ACCOUNTS : ROLE_SOURCE_ENTITLEMENT;
}

static void role_watch_release( struct role_watch *watch )
{
    if (InterlockedDecrement(&watch->refs)) return;
    if (watch->queue) IXThreadingImpl_XTaskQueueCloseHandle(x_threading_impl,watch->queue);
    DeleteCriticalSection(&watch->lock);
    free(watch);
}

static void CALLBACK role_watch_notify( void *context, BOOLEAN cancelled )
{
    struct role_notification *notification = context;
    struct role_watch *watch = notification->watch;
    XUserLocalId id = local_user.local_id;
    BOOL deliver;

    EnterCriticalSection(&watch->lock);
    deliver = !cancelled && !watch->stopped;
    if (deliver) watch->callback_thread = GetCurrentThreadId();
    LeaveCriticalSection(&watch->lock);
    if (deliver)
    {
        if (watch->kind == XGR_ROLE_WATCH_USER)
            watch->user_callback(watch->context,id,notification->state ? XUserChangeEvent_SignedInAgain : XUserChangeEvent_SignedOut);
        else watch->store_callback(watch->context);
        EnterCriticalSection(&watch->lock);
        watch->callback_thread = 0;
        WakeAllConditionVariable(&watch->idle);
        LeaveCriticalSection(&watch->lock);
    }
    free(notification);
    role_watch_release(watch);
}

/* Compare a definitive state with what the watch last saw, and queue the
 * notification if it changed. A watch registered while the state was unknown
 * takes its first answer as the baseline. */
static void role_watch_apply( struct role_watch *watch, BOOL state )
{
    struct role_notification *notification;
    BOOL changed;
    HRESULT hr;

    EnterCriticalSection(&watch->lock);
    if (watch->stopped) { LeaveCriticalSection(&watch->lock); return; }
    changed = watch->known && state != watch->state;
    watch->known = TRUE;
    watch->state = state;
    if (!changed || (watch->kind == XGR_ROLE_WATCH_PACKAGE && state) ||
        !(notification = malloc(sizeof(*notification))))
    {
        LeaveCriticalSection(&watch->lock);
        return;
    }
    notification->watch = watch;
    notification->state = state;
    InterlockedIncrement(&watch->refs);
    /* Submit outside the lock: Immediate queues may invoke reentrantly. */
    LeaveCriticalSection(&watch->lock);
    hr = IXThreadingImpl_XTaskQueueSubmitCallback(x_threading_impl,watch->queue,XTaskQueuePort_Completion,notification,role_watch_notify);
    if (FAILED(hr)) { free(notification); role_watch_release(watch); }
}

static void role_watches_update( enum role_source source )
{
    struct role_watch **list = NULL, **grown, *watch;
    struct account_roles_params roles;
    SIZE_T count = 0, capacity = 0, i;
    UINT32 active;

    if (!role_state_peek(source,&roles,&active)) return;
    /* Take a reference on each watch and let go of the list before calling
     * out: a notification on an Immediate queue may unregister from inside. */
    AcquireSRWLockShared(&role_watches_lock);
    for (watch = role_watches; watch; watch = watch->next)
    {
        if (role_watch_source(watch->kind) != source) continue;
        if (count == capacity)
        {
            capacity = capacity ? capacity * 2 : 8;
            if (!(grown = realloc(list,capacity * sizeof(*list)))) break;
            list = grown;
        }
        InterlockedIncrement(&watch->refs);
        list[count++] = watch;
    }
    ReleaseSRWLockShared(&role_watches_lock);
    for (i = 0; i < count; i++)
    {
        role_watch_apply(list[i],source == ROLE_SOURCE_ACCOUNTS ? roles.player_signed_in : active);
        role_watch_release(list[i]);
    }
    free(list);
}

HRESULT xgr_role_watch_register( enum xgr_role_watch_kind kind, void *owner, XTaskQueueHandle queue,
    void *context, XUserChangeEventCallback *user_callback, XStoreGameLicenseChangedCallback *store_callback,
    int initial, XTaskQueueRegistrationToken *token )
{
    struct role_watch *watch;
    HRESULT hr;
    if (!token || (kind == XGR_ROLE_WATCH_USER ? !user_callback : !store_callback)) return E_INVALIDARG;
    if (!(watch = calloc(1,sizeof(*watch)))) return E_OUTOFMEMORY;
    if (queue && FAILED(hr = IXThreadingImpl_XTaskQueueDuplicateHandle(x_threading_impl,queue,&watch->queue)))
    { free(watch); return hr; }
    InitializeCriticalSection(&watch->lock);
    InitializeConditionVariable(&watch->idle);
    watch->refs = 1; /* the registration */
    watch->kind = kind; watch->owner = owner; watch->context = context;
    watch->user_callback = user_callback; watch->store_callback = store_callback;
    watch->known = initial >= 0; watch->state = initial > 0;
    AcquireSRWLockExclusive(&role_watches_lock);
    watch->token = ++role_watch_next_token;
    watch->next = role_watches; role_watches = watch;
    token->token = watch->token;
    ReleaseSRWLockExclusive(&role_watches_lock);
    role_source_want(role_watch_source(kind),1,NULL);
    return S_OK;
}

BOOLEAN xgr_role_watch_unregister( enum xgr_role_watch_kind kind, void *owner,
    XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    struct role_watch **link, *watch = NULL;
    BOOL idle;
    AcquireSRWLockExclusive(&role_watches_lock);
    for (link = &role_watches; *link; link = &(*link)->next)
        if ((*link)->kind == kind && (*link)->owner == owner && (*link)->token == token.token)
        { watch = *link; *link = watch->next; break; }
    ReleaseSRWLockExclusive(&role_watches_lock);
    if (!watch) return TRUE;
    role_source_want(role_watch_source(kind),-1,NULL);
    EnterCriticalSection(&watch->lock);
    watch->stopped = TRUE;
    while (wait && watch->callback_thread && watch->callback_thread != GetCurrentThreadId())
        SleepConditionVariableCS(&watch->idle,&watch->lock,INFINITE);
    idle = !watch->callback_thread;
    LeaveCriticalSection(&watch->lock);
    role_watch_release(watch);
    return wait || idle;
}

void xgr_role_watch_close( enum xgr_role_watch_kind kind, void *owner )
{
    struct role_watch *watch;
    XTaskQueueRegistrationToken token;
    for (;;)
    {
        token.token = 0;
        AcquireSRWLockShared(&role_watches_lock);
        for (watch = role_watches; watch; watch = watch->next)
            if (watch->kind == kind && watch->owner == owner) { token.token = watch->token; break; }
        ReleaseSRWLockShared(&role_watches_lock);
        if (!token.token) return;
        xgr_role_watch_unregister(kind,owner,token,TRUE);
    }
}

static HRESULT WINAPI x_user_XUserRegisterForChangeEvent( IXUserImpl6 *iface, XTaskQueueHandle queue, void *context, XUserChangeEventCallback *callback, XTaskQueueRegistrationToken *token )
{
    struct account_roles_params roles;

    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );
    if (account_roles_enabled())
        return xgr_role_watch_register(XGR_ROLE_WATCH_USER,NULL,queue,context,callback,NULL,
            role_state_get(ROLE_SOURCE_ACCOUNTS,ROLE_GET_SEED,&roles,NULL) ? (int)roles.player_signed_in : -1,token);
    /* The legacy local user never changes. */
    if (token) token->token = 1;
    return S_OK;
}

static BOOLEAN WINAPI x_user_XUserUnregisterForChangeEvent( IXUserImpl6 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, wait %d.\n", iface, wait );
    if (account_roles_enabled()) return xgr_role_watch_unregister(XGR_ROLE_WATCH_USER,NULL,token,wait);
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
    struct account_roles_params roles;

    TRACE( "iface %p, user %p.\n", iface, user );
    if (account_roles_enabled())
        return is_local_user(user) &&
            role_state_get(ROLE_SOURCE_ACCOUNTS,ROLE_GET_SEED|ROLE_GET_FRESH,&roles,NULL) && roles.same_store_user;
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
    char live[XGR_IDENTITY_MAX];
    const char *value;
    SIZE_T len;

    TRACE( "iface %p, user %p, gamertagComponent %d, gamertagSize %Iu, gamertag %p.\n", iface, user, gamertagComponent, gamertagSize, gamertag );
    if (!gamertag) return E_INVALIDARG;

    /* Report the signed-in account's own gamertag when there is one. A classic
     * gamertag has no modern suffix, so every component but the suffix is just
     * the tag itself. */
    if (get_live_identity( NULL, live, sizeof(live) ) && live[0])
    {
        value = (gamertagComponent == XUserGamertagComponent_ModernSuffix) ? "" : live;
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
