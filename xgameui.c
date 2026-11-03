/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XGameUI
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

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

/*
 * The system UI, offline and headless: there is no overlay to show and no user
 * to click it, so each dialog completes immediately with the outcome a
 * dismissed dialog would give -- the default button for a message box, an empty
 * pick for a player picker, a cancelled entry for text input. Web
 * authentication cannot happen offline, so it reports cancellation rather than
 * hanging. Returning these instead of E_NOTIMPL keeps a title that waits on a
 * dialog from deadlocking on its own splash screen.
 */

struct x_text_entry
{
    LONG dummy;
};

struct x_game_ui
{
    IXGameUiImpl4 IXGameUiImpl4_iface;
    LONG ref;
};

static inline struct x_game_ui *impl_from_IXGameUiImpl4( IXGameUiImpl4 *iface )
{
    return CONTAINING_RECORD( iface, struct x_game_ui, IXGameUiImpl4_iface );
}

static HRESULT WINAPI x_game_ui_QueryInterface( IXGameUiImpl4 *iface, REFIID iid, void **out )
{
    struct x_game_ui *impl = impl_from_IXGameUiImpl4( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown      ) ||
        IsEqualGUID( iid, &IID_IXGameUiImpl  ) ||
        IsEqualGUID( iid, &IID_IXGameUiImpl2 ) ||
        IsEqualGUID( iid, &IID_IXGameUiImpl3 ) ||
        IsEqualGUID( iid, &IID_IXGameUiImpl4 ))
    {
        IXGameUiImpl4_AddRef( *out = &impl->IXGameUiImpl4_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_game_ui_AddRef( IXGameUiImpl4 *iface )
{
    struct x_game_ui *impl = impl_from_IXGameUiImpl4( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_game_ui_Release( IXGameUiImpl4 *iface )
{
    struct x_game_ui *impl = impl_from_IXGameUiImpl4( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_game_ui_XGameUiShowMessageDialogAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, const char *titleText, const char *contentText, const char *firstButtonText, const char *secondButtonText, const char *thirdButtonText, XGameUiMessageDialogButton defaultButton, XGameUiMessageDialogButton cancelButton )
{
    TRACE( "iface %p, async %p, titleText %s, contentText %s, defaultButton %d.\n", iface, async, debugstr_a( titleText ), debugstr_a( contentText ), defaultButton );
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, (void *)(ULONG_PTR)defaultButton );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiShowMessageDialogResult( IXGameUiImpl4 *iface, XAsyncBlock *async, XGameUiMessageDialogButton *resultButton )
{
    void *payload;
    HRESULT hr;

    TRACE( "iface %p, async %p, resultButton %p.\n", iface, async, resultButton );
    if (!xgr_async_completed( async, &hr, &payload )) return E_PENDING;
    if (resultButton) *resultButton = (XGameUiMessageDialogButton)(ULONG_PTR)payload;
    return hr;
}

static HRESULT WINAPI x_game_ui_XGameUiShowSendGameInviteAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser, const char *sessionConfigurationId, const char *sessionTemplateName, const char *sessionId, const char *invitationText, const char *customActivationContext )
{
    TRACE( "iface %p, async %p, requestingUser %p.\n", iface, async, requestingUser );
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiShowSendGameInviteResult( IXGameUiImpl4 *iface, XAsyncBlock *async )
{
    HRESULT hr;
    TRACE( "iface %p, async %p.\n", iface, async );
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    return hr;
}

static HRESULT WINAPI x_game_ui_XGameUiShowPlayerProfileCardAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser, UINT64 targetPlayer )
{
    TRACE( "iface %p, async %p, requestingUser %p, targetPlayer %I64u.\n", iface, async, requestingUser, targetPlayer );
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiShowPlayerProfileCardResult( IXGameUiImpl4 *iface, XAsyncBlock *async )
{
    HRESULT hr;
    TRACE( "iface %p, async %p.\n", iface, async );
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    return hr;
}

static HRESULT WINAPI x_game_ui_XGameUiShowAchievementsAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser, UINT32 titleId )
{
    TRACE( "iface %p, async %p, requestingUser %p, titleId %u.\n", iface, async, requestingUser, titleId );
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiShowAchievementsResult( IXGameUiImpl4 *iface, XAsyncBlock *async )
{
    HRESULT hr;
    TRACE( "iface %p, async %p.\n", iface, async );
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    return hr;
}

static HRESULT WINAPI x_game_ui_XGameUiShowPlayerPickerAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser, const char *promptText, UINT32 selectFromPlayersCount, const UINT64 *selectFromPlayers, UINT32 preSelectedPlayersCount, UINT64 *preSelectedPlayers, UINT32 minSelectionCount, UINT32 maxSelectionCount )
{
    TRACE( "iface %p, async %p, requestingUser %p, promptText %s.\n", iface, async, requestingUser, debugstr_a( promptText ) );
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiShowPlayerPickerResultCount( IXGameUiImpl4 *iface, XAsyncBlock *async, UINT32 *resultPlayersCount )
{
    HRESULT hr;
    TRACE( "iface %p, async %p, resultPlayersCount %p.\n", iface, async, resultPlayersCount );
    if (!resultPlayersCount) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    *resultPlayersCount = 0;
    return hr;
}

static HRESULT WINAPI x_game_ui_XGameUiShowPlayerPickerResult( IXGameUiImpl4 *iface, XAsyncBlock *async, UINT32 resultPlayersCount, UINT64 *resultPlayers, UINT32 *resultPlayersUsed )
{
    HRESULT hr;
    TRACE( "iface %p, async %p, resultPlayersCount %u, resultPlayersUsed %p.\n", iface, async, resultPlayersCount, resultPlayersUsed );
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    if (resultPlayersUsed) *resultPlayersUsed = 0;
    return hr;
}

static HRESULT WINAPI x_game_ui_XGameUiShowErrorDialogAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, HRESULT errorCode, const char *context )
{
    TRACE( "iface %p, async %p, errorCode %#lx, context %s.\n", iface, async, errorCode, debugstr_a( context ) );
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiShowErrorDialogResult( IXGameUiImpl4 *iface, XAsyncBlock *async )
{
    HRESULT hr;
    TRACE( "iface %p, async %p.\n", iface, async );
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    return hr;
}

static HRESULT WINAPI x_game_ui_XGameUiSetNotificationPositionHint( IXGameUiImpl4 *iface, XGameUiNotificationPositionHint position )
{
    TRACE( "iface %p, position %d.\n", iface, position );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiShowTextEntryAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, const char *titleText, const char *descriptionText, const char *defaultText, XGameUiTextEntryInputScope inputScope, UINT32 maxTextLength )
{
    TRACE( "iface %p, async %p, titleText %s, inputScope %d, maxTextLength %u.\n", iface, async, debugstr_a( titleText ), inputScope, maxTextLength );
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiShowTextEntryResultSize( IXGameUiImpl4 *iface, XAsyncBlock *async, UINT32 *resultTextBufferSize )
{
    HRESULT hr;
    TRACE( "iface %p, async %p, resultTextBufferSize %p.\n", iface, async, resultTextBufferSize );
    if (!resultTextBufferSize) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    *resultTextBufferSize = 1; /* empty string: just the NUL */
    return hr;
}

static HRESULT WINAPI x_game_ui_XGameUiShowTextEntryResult( IXGameUiImpl4 *iface, XAsyncBlock *async, UINT32 resultTextBufferSize, char *resultTextBuffer, UINT32 *resultTextBufferUsed )
{
    HRESULT hr;
    TRACE( "iface %p, async %p, resultTextBufferSize %u, resultTextBuffer %p.\n", iface, async, resultTextBufferSize, resultTextBuffer );
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    if (resultTextBuffer && resultTextBufferSize) resultTextBuffer[0] = 0;
    if (resultTextBufferUsed) *resultTextBufferUsed = 1;
    return hr;
}

static HRESULT WINAPI __PADDING__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_2__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_3__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_4__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiShowWebAuthenticationAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser, const char *requestUri, const char *completionUri )
{
    TRACE( "iface %p, async %p, requestingUser %p, requestUri %s.\n", iface, async, requestingUser, debugstr_a( requestUri ) );
    if (!async) return E_INVALIDARG;
    /* web auth needs a live browser and Xbox Live; offline it is cancelled */
    xgr_async_complete( async, E_ABORT, NULL );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiShowWebAuthenticationResultSize( IXGameUiImpl4 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    HRESULT hr;
    TRACE( "iface %p, async %p, bufferSize %p.\n", iface, async, bufferSize );
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    if (bufferSize) *bufferSize = 0;
    return hr;
}

static HRESULT WINAPI x_game_ui_XGameUiShowWebAuthenticationResult( IXGameUiImpl4 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, XGameUiWebAuthenticationResultData **ptrToBuffer, SIZE_T *bufferUsed )
{
    HRESULT hr;
    TRACE( "iface %p, async %p, bufferSize %Iu, buffer %p.\n", iface, async, bufferSize, buffer );
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    return hr;
}

static HRESULT WINAPI x_game_ui_XGameUiShowWebAuthenticationWithOptionsAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser, const char *requestUri, const char *completionUri, XGameUiWebAuthenticationOptions options )
{
    TRACE( "iface %p, async %p, requestingUser %p, requestUri %s, options %#x.\n", iface, async, requestingUser, debugstr_a( requestUri ), options );
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, E_ABORT, NULL );
    return S_OK;
}

static HRESULT WINAPI __PADDING_5__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_6__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiShowMultiplayerActivityGameInviteAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser )
{
    TRACE( "iface %p, async %p, requestingUser %p.\n", iface, async, requestingUser );
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiShowMultiplayerActivityGameInviteResult( IXGameUiImpl4 *iface, XAsyncBlock *async )
{
    HRESULT hr;
    TRACE( "iface %p, async %p.\n", iface, async );
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    return hr;
}

static HRESULT WINAPI __PADDING_7__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_8__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiTextEntryOpen( IXGameUiImpl4 *iface, const XGameUiTextEntryOptions *options, UINT32 maxLength, const char *initialText, UINT32 initialCursorIndex, XGameUiTextEntryHandle *handle )
{
    struct x_text_entry *entry;
    TRACE( "iface %p, options %p, maxLength %u, initialText %s, handle %p.\n", iface, options, maxLength, debugstr_a( initialText ), handle );
    if (!handle) return E_INVALIDARG;
    if (!(entry = calloc( 1, sizeof(*entry) ))) return E_OUTOFMEMORY;
    *handle = (XGameUiTextEntryHandle)entry;
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiTextEntryClose( IXGameUiImpl4 *iface, XGameUiTextEntryHandle handle )
{
    TRACE( "iface %p, handle %p.\n", iface, handle );
    free( handle );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiTextEntryGetState( IXGameUiImpl4 *iface, XGameUiTextEntryHandle handle, XGameUiTextEntryChangeTypeFlags *changeType, UINT32 *cursorIndex, UINT32 *imeClauseStartIndex, UINT32 *imeClauseEndIndex, UINT32 bufferSize, char *buffer )
{
    TRACE( "iface %p, handle %p, bufferSize %u, buffer %p.\n", iface, handle, bufferSize, buffer );
    if (changeType) *changeType = (XGameUiTextEntryChangeTypeFlags)0;
    if (cursorIndex) *cursorIndex = 0;
    if (imeClauseStartIndex) *imeClauseStartIndex = 0;
    if (imeClauseEndIndex) *imeClauseEndIndex = 0;
    if (buffer && bufferSize) buffer[0] = 0;
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiTextEntryGetExtents( IXGameUiImpl4 *iface, XGameUiTextEntryHandle handle, XGameUiTextEntryExtents *extents )
{
    TRACE( "iface %p, handle %p, extents %p.\n", iface, handle, extents );
    if (extents) memset( extents, 0, sizeof(*extents) );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiTextEntryUpdatePositionHint( IXGameUiImpl4 *iface, XGameUiTextEntryHandle handle, XGameUiTextEntryPositionHint positionHint )
{
    TRACE( "iface %p, handle %p, positionHint %d.\n", iface, handle, positionHint );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiTextEntryUpdateVisibility( IXGameUiImpl4 *iface, XGameUiTextEntryHandle handle, XGameUiTextEntryVisibilityFlags visibilityFlags )
{
    TRACE( "iface %p, handle %p, visibilityFlags %#x.\n", iface, handle, visibilityFlags );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiShowStateShareAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser, const char *linkToken )
{
    TRACE( "iface %p, async %p, requestingUser %p, linkToken %s.\n", iface, async, requestingUser, debugstr_a( linkToken ) );
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiShowStateShareResult( IXGameUiImpl4 *iface, XAsyncBlock *async )
{
    HRESULT hr;
    TRACE( "iface %p, async %p.\n", iface, async );
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    return hr;
}

static HRESULT WINAPI x_game_ui_XGameUiSetUiCallbacks( IXGameUiImpl4 *iface, const XGameUiUiCallbacks *callbacks, BOOLEAN useSystemUiIfAvailable )
{
    TRACE( "iface %p, callbacks %p, useSystemUiIfAvailable %d.\n", iface, callbacks, useSystemUiIfAvailable );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiSetMessageDialogUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle, XGameUiMessageDialogButton response )
{
    TRACE( "iface %p, callbackHandle %p, response %d.\n", iface, callbackHandle, response );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiSetPlayerPickerUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle, UINT32 playerCount, const UINT64 *players )
{
    TRACE( "iface %p, callbackHandle %p, playerCount %u.\n", iface, callbackHandle, playerCount );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiSetTextEntryUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle, const char *response )
{
    TRACE( "iface %p, callbackHandle %p, response %s.\n", iface, callbackHandle, debugstr_a( response ) );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiSetPlayerProfileCardUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle )
{
    TRACE( "iface %p, callbackHandle %p.\n", iface, callbackHandle );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiSetSendGameInviteUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle )
{
    TRACE( "iface %p, callbackHandle %p.\n", iface, callbackHandle );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiSetAchievementsUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle )
{
    TRACE( "iface %p, callbackHandle %p.\n", iface, callbackHandle );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiSetMultiplayerActivityGameInviteUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle )
{
    TRACE( "iface %p, callbackHandle %p.\n", iface, callbackHandle );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiSetErrorDialogUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle )
{
    TRACE( "iface %p, callbackHandle %p.\n", iface, callbackHandle );
    return S_OK;
}

static const struct IXGameUiImpl4Vtbl x_game_ui_vtbl =
{
    x_game_ui_QueryInterface,
    x_game_ui_AddRef,
    x_game_ui_Release,
    /* IXGameUiImpl methods */
    x_game_ui_XGameUiShowMessageDialogAsync,
    x_game_ui_XGameUiShowMessageDialogResult,
    x_game_ui_XGameUiShowSendGameInviteAsync,
    x_game_ui_XGameUiShowSendGameInviteResult,
    x_game_ui_XGameUiShowPlayerProfileCardAsync,
    x_game_ui_XGameUiShowPlayerProfileCardResult,
    x_game_ui_XGameUiShowAchievementsAsync,
    x_game_ui_XGameUiShowAchievementsResult,
    x_game_ui_XGameUiShowPlayerPickerAsync,
    x_game_ui_XGameUiShowPlayerPickerResultCount,
    x_game_ui_XGameUiShowPlayerPickerResult,
    x_game_ui_XGameUiShowErrorDialogAsync,
    x_game_ui_XGameUiShowErrorDialogResult,
    x_game_ui_XGameUiSetNotificationPositionHint,
    x_game_ui_XGameUiShowTextEntryAsync,
    x_game_ui_XGameUiShowTextEntryResultSize,
    x_game_ui_XGameUiShowTextEntryResult,
    __PADDING__,
    __PADDING_2__,
    __PADDING_3__,
    __PADDING_4__,
    x_game_ui_XGameUiShowWebAuthenticationAsync,
    x_game_ui_XGameUiShowWebAuthenticationResultSize,
    x_game_ui_XGameUiShowWebAuthenticationResult,
    x_game_ui_XGameUiShowWebAuthenticationWithOptionsAsync,
    __PADDING_5__,
    __PADDING_6__,
    /* IXGameUiImpl2 methods */
    x_game_ui_XGameUiShowMultiplayerActivityGameInviteAsync,
    x_game_ui_XGameUiShowMultiplayerActivityGameInviteResult,
    __PADDING_7__,
    __PADDING_8__,
    x_game_ui_XGameUiTextEntryOpen,
    x_game_ui_XGameUiTextEntryClose,
    x_game_ui_XGameUiTextEntryGetState,
    x_game_ui_XGameUiTextEntryGetExtents,
    x_game_ui_XGameUiTextEntryUpdatePositionHint,
    x_game_ui_XGameUiTextEntryUpdateVisibility,
    /* IXGameUiImpl3 methods */
    x_game_ui_XGameUiShowStateShareAsync,
    x_game_ui_XGameUiShowStateShareResult,
    /* IXGameUiImpl4 methods */
    x_game_ui_XGameUiSetUiCallbacks,
    x_game_ui_XGameUiSetMessageDialogUiResponse,
    x_game_ui_XGameUiSetPlayerPickerUiResponse,
    x_game_ui_XGameUiSetTextEntryUiResponse,
    x_game_ui_XGameUiSetPlayerProfileCardUiResponse,
    x_game_ui_XGameUiSetSendGameInviteUiResponse,
    x_game_ui_XGameUiSetAchievementsUiResponse,
    x_game_ui_XGameUiSetMultiplayerActivityGameInviteUiResponse,
    x_game_ui_XGameUiSetErrorDialogUiResponse,
};

static struct x_game_ui x_game_ui =
{
    {&x_game_ui_vtbl},
    0,
};

IXGameUiImpl *x_game_ui_impl = (IXGameUiImpl *)&x_game_ui.IXGameUiImpl4_iface;
