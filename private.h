/*
 * Xbox Game runtime Library
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

#define COBJMACROS

#include <roapi.h>
#include <wine/debug.h>
#include <winstring.h>

#include <xaccessibility.h>
#include <xappcapture.h>
#include <xasync.h>
#include <xasyncprovider.h>
#include <xdisplay.h>
#include <xerror.h>
#include <xgame.h>
#include <xgameactivation.h>
#include <xgameerr.h>
#include <xgameevent.h>
#include <xgameinvite.h>
#include <xgameprotocol.h>
#include <xgameruntimefeature.h>
#include <xgameruntimetypes.h>
#include <xgamesave.h>
#include <xgamestreaming.h>
#include <xgameui.h>
#include <xnetworking.h>
#include <xpackage.h>
#include <xpersistentlocalstorage.h>
#include <xspeechsynthesizer.h>
#include <xstore.h>
#include <xsystem.h>
#include <xtaskqueue.h>
#include <xuser.h>

/* April 2026 Release of GDK */
#define GDKC_VERSION 10002L
#define GAMING_SERVICES_VERSION 7822L

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include <windows.foundation.h>
#define WIDL_using_Windows_Globalization
#include <windows.globalization.h>
#define WIDL_using_Windows_System_Profile
#include <windows.system.profile.h>

extern DWORD tlsIndex;

extern IXAccessibilityImpl *x_accessibility_impl;
extern IXAppCaptureImpl *x_app_capture_impl;
extern IXAppCaptureMetadataImpl *x_app_capture_metadata_impl;
extern IXDisplayImpl *x_display_impl;
extern IXErrorImpl *x_error_impl;
extern IXGameImpl *x_game_impl;
extern IXGameActivationImpl *x_game_activation_impl;
extern IXGameEventImpl *x_game_event_impl;
extern IXGameInviteImpl *x_game_invite_impl;
extern IXGameProtocolImpl *x_game_protocol_impl;
extern IXGameRuntimeFeatureImpl *x_game_runtime_feature_impl;
extern IXGameSaveImpl *x_game_save_impl;
extern IXGameStreamingImpl *x_game_streaming_impl;
extern IXGameUiImpl *x_game_ui_impl;
extern IXLauncherImpl *x_launcher_impl;
extern IXNetworkingImpl *x_networking_impl;
extern IXPackageImpl *x_package_impl;
extern IXPersistentLocalStorageImpl *x_persistent_local_storage_impl;
extern IXStoreImpl *x_store_impl;
extern IXSystemImpl *x_system_impl;
extern IXSystemAnalyticsImpl *x_system_analytics_impl;
extern IXThreadingImpl *x_threading_impl;
extern IXUserImpl *x_user_impl;
extern IXUserDeviceImpl *x_user_device_impl;

HRESULT WINAPI QueryApiImpl( const GUID *classId, REFIID interfaceId, void **out );

/* Minimal XAsync completion state.
 *
 * The GDK async pattern is: the caller fills an XAsyncBlock, an *Async() entry
 * point starts the work, and once it finishes the block's callback runs and the
 * caller retrieves the outcome with a matching *Result() call (or polls with
 * XAsyncGetStatus). Operations that can be satisfied locally have no work to
 * schedule, so they complete inline: record the result in the block's opaque
 * internal[] slots and invoke the callback before returning.
 */
#define XGR_ASYNC_MAGIC ((void *)(ULONG_PTR)0x53554458) /* 'XDUS' */

/* Record the outcome and queue the completion callback.
 *
 * The callback must not run here: callers expect it while they are pumping
 * their task queue (XTaskQueueDispatch), not re-entrantly from inside the
 * *Async() call they are still making. Delivering it inline leaves titles
 * spinning in their dispatch loop waiting for a completion that already fired.
 */
void xgr_async_complete( XAsyncBlock *async, HRESULT hr, void *payload );
/* Same, also recording the buffer size XAsyncGetResultSize should report. */
void xgr_async_complete_ex( XAsyncBlock *async, HRESULT hr, void *payload, SIZE_T result_size );
/* From a provider's DoWork: run `callback` off the thread that dispatched it,
 * counted as the block's queue's work, and let it complete the operation. On
 * failure nothing runs and the caller does the work itself. */
HRESULT xgr_async_run_detached( XAsyncBlock *async, PTP_SIMPLE_CALLBACK callback, void *context );

/* Inline MicrosoftGame.config text from InitializeApiImplEx2, or NULL. */
extern char *xgr_game_config;

/* XUID of the single local user (0 when unknown, see XGR_XUID). */
UINT64 xgr_local_xuid( void );

/* Derived package family name (UTF-8, malloc'd) and install-root directory
 * (WCHAR, malloc'd), shared between XGameSave and XPackage. */
char *xgr_package_family( void );
const char *xgr_current_package_identifier( void );
WCHAR *xgr_package_root( void );

/* MicrosoftGame.config text (inline or from disk; UTF-8, malloc'd, or NULL). */
char *xgr_load_game_config( void );

/* Read one value out of MicrosoftGame.config, which is parsed and cached once
 * in xpackage.c: the text of <element>text</element>, or attr="value" from the
 * first <element ...> found. FALSE, and an empty `out`, if it is not there.
 * The config is the only description of the title we have offline, so what a
 * package, a store product and a save folder say about it all comes from
 * here and therefore agrees. */
BOOL xgr_config_element( const char *element, char *out, SIZE_T cap );
BOOL xgr_config_attr( const char *element, const char *attr, char *out, SIZE_T cap );

/* The running title's identity for Xbox authentication, from its
 * MicrosoftGame.config: `TitleId` (rendered decimal, since the config carries
 * it in hexadecimal and Xbox wants decimal) and `MSAAppId`. Either comes back
 * empty when the config does not carry it.
 *
 * Xbox mints the XSTS *title claim* from the pair, and the claim is not a
 * formality: a service that scopes what it returns by title -- presence's
 * titles/current, the session directory -- answers 403 to a token claiming a
 * different title just as flatly as to one claiming none. */
void xgr_title_identity( char *title_id, SIZE_T title_id_len, char *client_id, SIZE_T client_id_len );

/* Wrap a UTF-8 filesystem path in an XPackageMountHandle (from xpackage.c). */
void *xgr_make_mount( const char *path );

/* Write the game saves still queued for the background writer (xgamesave.c);
 * called once at process exit. */
void xgr_save_process_exit( void );

static inline BOOL xgr_async_completed( const XAsyncBlock *async, HRESULT *hr, void **payload )
{
    if (!async || async->internal[0] != XGR_ASYNC_MAGIC) return FALSE;
    if (hr) *hr = (HRESULT)(ULONG_PTR)async->internal[1];
    if (payload) *payload = async->internal[2];
    return TRUE;
}

/* Account-scoped change notifications (inactive in legacy runtime mode). */
enum xgr_role_watch_kind { XGR_ROLE_WATCH_USER, XGR_ROLE_WATCH_GAME, XGR_ROLE_WATCH_PACKAGE };
/* `initial` is the state the caller last reported (0 or 1), or -1 when it is
 * not known yet and the first definitive answer becomes the baseline. */
HRESULT xgr_role_watch_register(enum xgr_role_watch_kind, void *, XTaskQueueHandle, void *,
    XUserChangeEventCallback *, XStoreGameLicenseChangedCallback *, int, XTaskQueueRegistrationToken *);
BOOLEAN xgr_role_watch_unregister(enum xgr_role_watch_kind, void *, XTaskQueueRegistrationToken, BOOLEAN);
void xgr_role_watch_close(enum xgr_role_watch_kind, void *);
/* The Store account's entitlement to the running game as last answered by the
 * service, kept off the title's threads (xuser.c). FALSE while nothing
 * definitive is known. With `fresh`, a question after a quiet spell waits a
 * bounded time for a current answer (see role_state_get). Store requests that
 * ask the service themselves feed their answer back with the unix call's
 * status. */
BOOL xgr_role_entitlement( UINT32 *active, BOOL fresh );
void xgr_role_entitlement_store( LONG status /* NTSTATUS */, UINT32 active );
