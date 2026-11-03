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
#include <rtlsupportapi.h>

WINE_DEFAULT_DEBUG_CHANNEL(xgameruntime);
WINE_DECLARE_DEBUG_CHANNEL(hctrace);
WINE_DECLARE_DEBUG_CHANNEL(xgrfault);

/* Opt-in read-only exception diagnostics. Games may deliberately raise a
 * large number of handled illegal-instruction exceptions; tracing all SEH
 * activity changes timing substantially. Capture only memory faults here. */
static PVOID fault_trace_handler;

static LONG CALLBACK trace_memory_fault(EXCEPTION_POINTERS *exception)
{
#ifdef __x86_64__
    static LONG tracing, count;
    CONTEXT context = *exception->ContextRecord;
    ULONG_PTR frames[24], instruction[4];
    SIZE_T read;
    ULONG_PTR image_base;
    PRUNTIME_FUNCTION function;
    PVOID handler_data;
    ULONG_PTR establisher;
    unsigned int i, n = 0;
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        InterlockedCompareExchange(&tracing, 1, 0)) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&count) <= 64)
    {
        TRACE_(xgrfault)("memory fault rip=%s rsp=%s rax=%s rbx=%s rcx=%s rdx=%s rsi=%s rdi=%s.\n",
                        wine_dbgstr_longlong(context.Rip), wine_dbgstr_longlong(context.Rsp),
                        wine_dbgstr_longlong(context.Rax), wine_dbgstr_longlong(context.Rbx),
                        wine_dbgstr_longlong(context.Rcx), wine_dbgstr_longlong(context.Rdx),
                        wine_dbgstr_longlong(context.Rsi), wine_dbgstr_longlong(context.Rdi));
        if (ReadProcessMemory(GetCurrentProcess(), (void *)context.Rip, instruction, sizeof(instruction), &read))
            for (i = 0; i < ARRAY_SIZE(instruction); i++)
                TRACE_(xgrfault)("fault instruction word %u: %s.\n", i, wine_dbgstr_longlong(instruction[i]));
        while (context.Rip && n < ARRAY_SIZE(frames))
        {
            frames[n++] = context.Rip;
            function = RtlLookupFunctionEntry(context.Rip, &image_base, NULL);
            if (function)
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function,
                                 &context, &handler_data, &establisher, NULL);
            else
            {
                if (!ReadProcessMemory(GetCurrentProcess(), (void *)context.Rsp, &context.Rip, sizeof(context.Rip), &read)) break;
                context.Rsp += sizeof(context.Rip);
            }
        }
        for (i = 0; i < n; i++) TRACE_(xgrfault)("frame %u: %s.\n", i, wine_dbgstr_longlong(frames[i]));
    }
    InterlockedExchange(&tracing, 0);
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}

static void enable_fault_trace(void)
{
    static LONG enabled;
    if (getenv("XGR_TRACE_FAULTS") && !InterlockedCompareExchange(&enabled, 1, 0))
    {
        fault_trace_handler = AddVectoredExceptionHandler(1, trace_memory_fault);
        TRACE_(xgrfault)("fault observer registered: %p.\n", fault_trace_handler);
        if (!fault_trace_handler) InterlockedExchange(&enabled, 0);
    }
}

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
            if (fault_trace_handler) RemoveVectoredExceptionHandler(fault_trace_handler);
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
    TRACE_(hctrace)( "libhttpclient: %s\n", debugstr_a( message ) );
}

static DWORD WINAPI hc_trace_thread( void *arg )
{
    void (WINAPI *set_callback)( void (CALLBACK *)( const char *, UINT32, UINT64, UINT64, const char * ) );
    HRESULT (WINAPI *set_level)( UINT32 );
    HMODULE mod = NULL;
    int i;

    for (i = 0; i < 1200 && !mod; i++)
    {
        mod = GetModuleHandleA( "libHttpClient.GDK.dll" );
        if (!mod) mod = GetModuleHandleA( "libHttpClient.dll" );
        if (!mod) Sleep( 100 );
    }
    if (!mod) { WARN( "libHttpClient never loaded; no trace.\n" ); return 0; }

    set_callback = (void *)GetProcAddress( mod, "HCTraceSetClientCallback" );
    set_level = (void *)GetProcAddress( mod, "HCSettingsSetTraceLevel" );
    if (!set_callback || !set_level) { WARN( "libHttpClient has no trace entry points.\n" ); return 0; }

    set_callback( hc_trace_callback );
    set_level( 5 );   /* Verbose */
    TRACE_(hctrace)( "libHttpClient tracing enabled.\n" );
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

/* XSystemGetRuntimeInfo is implemented in the statically linked GDK client.
 * On desktop it reads these REG_QWORD values directly, including before API
 * initialization. Their layout is package-version order (major in the high
 * word), not XVersion's in-memory word order. Without registration the SDK
 * reports zero, and Forza Horizon 6 refuses Start Game before it can exercise
 * any other compatibility behavior. This is the Gaming Services package contract version (35.112.20003.0),
 * distinct from the 10.0.x GDK/DLL interface versions passed to initialization;
 * it does not assert that every API or title has been qualified. */
HRESULT WINAPI DllRegisterServer( void )
{
    static const WCHAR key_name[] = L"Software\\Microsoft\\GamingServices";
    const UINT64 version = ((UINT64)35 << 48) | ((UINT64)112 << 32) | ((UINT64)20003 << 16);
    HKEY key;
    LONG status;

    if ((status = RegCreateKeyExW( HKEY_LOCAL_MACHINE, key_name, 0, NULL, 0,
                                  KEY_SET_VALUE, NULL, &key, NULL )))
        return HRESULT_FROM_WIN32( status );
    status = RegSetValueExW( key, L"CurrentVersion", 0, REG_QWORD,
                            (const BYTE *)&version, sizeof(version) );
    if (!status)
        status = RegSetValueExW( key, L"AvailableVersion", 0, REG_QWORD,
                                (const BYTE *)&version, sizeof(version) );
    RegCloseKey( key );
    /* Advertise the PFX facade only to a title that has the matching native
     * save backend loaded. Legacy titles keep their previous discovery path. */
    if (!status && GetModuleHandleW(L"PlayFabGameSave.dll") && GetModuleHandleW(L"PlayFabCore.dll"))
    {
        status = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OneSettings\\xbox\\GamingRuntimeServices\\Settings",
                0, NULL, 0, KEY_QUERY_VALUE, NULL, &key, NULL);
        if (!status) RegCloseKey(key);
    }
    return status ? HRESULT_FROM_WIN32( status ) : S_OK;
}

HRESULT WINAPI InitializeApiImplEx2( UINT64 gdkVer, UINT64 gsVer, char mode, const struct initialize_options *options )
{
    HRESULT registration;
    enable_hc_trace_once();
    enable_fault_trace();
    TRACE( "gdkVer %s, gsVer %s, mode %d, options %p.\n",
           wine_dbgstr_longlong(gdkVer), wine_dbgstr_longlong(gsVer), mode, options );
    if (FAILED(registration = DllRegisterServer()))
        WARN( "runtime discovery registration failed, hr %#lx.\n", registration );

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

HRESULT WINAPI InitializeApiImplEx( UINT64 gdkVer, UINT64 gsVer, char mode )
{
    return InitializeApiImplEx2( gdkVer, gsVer, mode, NULL );
}

HRESULT WINAPI InitializeApiImpl( UINT64 gdkVer, UINT64 gsVer )
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
