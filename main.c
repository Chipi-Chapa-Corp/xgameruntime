/*
 * Xbox Game runtime Library
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

#include <initguid.h>
#include "private.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(xgameruntime);

DWORD tlsIndex;

BOOL WINAPI DllMain( HINSTANCE hinst, DWORD reason, void *reserved )
{
    TRACE( "hinst %p, reason %lu, reserved %p.\n", hinst, reason, reserved );

    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            if ((tlsIndex = TlsAlloc()) == TLS_OUT_OF_INDEXES) return FALSE;
            /* Needed before any WINE_UNIX_CALL. A failure here only costs the
             * Xbox Live sign-in (the unix side talks to xodus-service), so warn
             * and carry on rather than failing the whole module load. */
            if (__wine_init_unix_call())
                WARN( "no unixlib; Xbox Live sign-in will be unavailable.\n" );
        case DLL_THREAD_ATTACH:
            TlsSetValue( tlsIndex, FALSE );
            break;
        case DLL_PROCESS_DETACH:
            TlsFree( tlsIndex );
            break;
    }
    return TRUE;
}

struct initialize_options
{
    UINT32 unk;
    BOOL isInline;
    const char *gameConfig;
};

/* MicrosoftGame.config text handed over at initialisation, when the title
 * carries its configuration inline instead of as a file next to the binary. */
char *xgr_game_config;


/* libHttpClient keeps its own trace log, and it is the only view we have of
 * what happens to a WebSocket inside the title -- the freeze when joining a
 * server from the in-game list ends with an HC_WEBSOCKET_OBSERVER destructor
 * running against an already-destroyed WebSocket, and libHttpClient narrates
 * exactly that. It is not our DLL, so this only switches its tracing on and
 * forwards it into the Wine log, and only when asked: set XGDK_HC_TRACE=1.
 *
 * libHttpClient loads after us, so this waits for it rather than assuming. */
static void CALLBACK hc_trace_callback( const char *area, UINT32 level, UINT64 thread,
                                        UINT64 timestamp, const char *message )
{
    TRACE( "libhttpclient: %s\n", debugstr_a( message ) );
}

static DWORD WINAPI hc_trace_thread( void *arg )
{
    void (WINAPI *set_callback)( void (CALLBACK *)( const char *, UINT32, UINT64, UINT64, const char * ) );
    HRESULT (WINAPI *set_level)( UINT32 );
    HMODULE mod = NULL;
    int i;

    for (i = 0; i < 1200 && !mod; i++)
    {
        if (!(mod = GetModuleHandleA( "libHttpClient.GDK.dll" ))) Sleep( 100 );
    }
    if (!mod) { WARN( "libHttpClient never loaded; no trace.\n" ); return 0; }

    set_callback = (void *)GetProcAddress( mod, "HCTraceSetClientCallback" );
    set_level = (void *)GetProcAddress( mod, "HCSettingsSetTraceLevel" );
    if (!set_callback || !set_level) { WARN( "libHttpClient has no trace entry points.\n" ); return 0; }

    set_callback( hc_trace_callback );
    set_level( 5 );   /* Verbose */
    TRACE( "libHttpClient tracing enabled.\n" );
    return 0;
}

static void enable_hc_trace_once(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;

    if (!InitOnceBeginInitialize( &once, 0, &pending, NULL ) || !pending) return;
    if (getenv( "XGDK_HC_TRACE" ))
    {
        HANDLE thread = CreateThread( NULL, 0, hc_trace_thread, NULL, 0, NULL );

        if (thread) CloseHandle( thread );
    }
    InitOnceComplete( &once, 0, NULL );
}

HRESULT WINAPI InitializeApiImplEx2( ULONG gdkVer, ULONG gsVer, char mode, const struct initialize_options *options )
{
    enable_hc_trace_once();
    TRACE( "gdkVer %ld, gsVer %ld, mode %d, options %p.\n", gdkVer, gsVer, mode, options );

    if (options)
    {
        TRACE( "options: unk %u, isInline %d, gameConfig %p.\n", options->unk, options->isInline, options->gameConfig );
        if (options->gameConfig && !xgr_game_config && !IsBadStringPtrA( options->gameConfig, 4 * 1024 * 1024 ))
        {
            SIZE_T len = strlen( options->gameConfig ) + 1;
            if ((xgr_game_config = malloc( len ))) memcpy( xgr_game_config, options->gameConfig, len );
        }
    }
    return S_OK;
}

HRESULT WINAPI InitializeApiImplEx( ULONG gdkVer, ULONG gsVer, char mode )
{
    return InitializeApiImplEx2( gdkVer, gsVer, mode, NULL );
}

HRESULT WINAPI InitializeApiImpl( ULONG gdkVer, ULONG gsVer )
{
    return InitializeApiImplEx2( gdkVer, gsVer, 0, NULL );
}

void WINAPI UninitializeApiImpl( void )
{
    /* Counterpart to InitializeApiImpl. Nothing is allocated at initialize
     * time, so there is nothing to tear down -- but the export must exist:
     * titles call it on shutdown, and a missing one raises
     * "Unimplemented function" and takes the process down with it. */
    TRACE( ".\n" );
}

HRESULT WINAPI QueryApiImpl( REFCLSID clsid, REFIID iid, void **out )
{
    TRACE( "clsid %s, iid %s, out %p.\n", debugstr_guid( clsid ), debugstr_guid( iid ), out );

    if (IsEqualGUID( clsid, &CLSID_XAccessibilityImpl ))
        return IXAccessibilityImpl_QueryInterface( x_accessibility_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XAppCaptureImpl ))
        return IXAppCaptureImpl_QueryInterface( x_app_capture_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XAppCaptureMetadataImpl ))
        return IXAppCaptureMetadataImpl_QueryInterface( x_app_capture_metadata_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XDisplayImpl ))
        return IXDisplayImpl_QueryInterface( x_display_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XErrorImpl ))
        return IXErrorImpl_QueryInterface( x_error_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XGameImpl ))
        return IXGameImpl_QueryInterface( x_game_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XGameActivationImpl ))
        return IXGameActivationImpl_QueryInterface( x_game_activation_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XGameEventImpl ))
        return IXGameEventImpl_QueryInterface( x_game_event_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XGameInviteImpl ))
        return IXGameInviteImpl_QueryInterface( x_game_invite_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XGameProtocolImpl ))
        return IXGameProtocolImpl_QueryInterface( x_game_protocol_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XGameRuntimeFeatureImpl ))
        return IXGameRuntimeFeatureImpl_QueryInterface( x_game_runtime_feature_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XGameSaveImpl ))
        return IXGameSaveImpl_QueryInterface( x_game_save_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XGameStreamingImpl ))
        return IXGameStreamingImpl_QueryInterface( x_game_streaming_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XGameUiImpl ))
        return IXGameUiImpl_QueryInterface( x_game_ui_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XLauncherImpl ))
        return IXLauncherImpl_QueryInterface( x_launcher_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XNetworkingImpl ))
        return IXNetworkingImpl_QueryInterface( x_networking_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XPackageImpl ))
        return IXPackageImpl_QueryInterface( x_package_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XPersistentLocalStorageImpl ))
        return IXPersistentLocalStorageImpl_QueryInterface( x_persistent_local_storage_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XStoreImpl ))
        return IXStoreImpl_QueryInterface( x_store_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XSystemImpl ))
        return IXSystemImpl_QueryInterface( x_system_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XSystemAnalyticsImpl ))
        return IXSystemAnalyticsImpl_QueryInterface( x_system_analytics_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XThreadingImpl ))
        return IXThreadingImpl_QueryInterface( x_threading_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XUserImpl ))
        return IXUserImpl_QueryInterface( x_user_impl, iid, out );
    if (IsEqualGUID( clsid, &CLSID_XUserDeviceImpl ))
        return IXUserDeviceImpl_QueryInterface( x_user_device_impl, iid, out );

    return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
}
