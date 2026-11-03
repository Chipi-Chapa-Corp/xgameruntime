/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XPersistentLocalStorage
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

struct x_persistent_local_storage
{
    IXPersistentLocalStorageImpl3 IXPersistentLocalStorageImpl3_iface;
    LONG ref;
};

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif

/* Persistent per-title storage: a real directory under the package's
 * LocalAppData that survives across runs, which is what the game expects. */
static char *pls_dir_utf8( void )
{
    WCHAR base[MAX_PATH], *family_w = NULL, path[MAX_PATH];
    char *family = xgr_package_family(), *ret = NULL;
    int i, n;

    if (!family) return NULL;
    if (!GetEnvironmentVariableW( L"LOCALAPPDATA", base, ARRAY_SIZE(base) ))
    {
        free( family );
        return NULL;
    }
    n = MultiByteToWideChar( CP_UTF8, 0, family, -1, NULL, 0 );
    if (n && (family_w = malloc( n * sizeof(WCHAR) )))
        MultiByteToWideChar( CP_UTF8, 0, family, -1, family_w, n );
    free( family );
    if (!family_w) return NULL;

    swprintf( path, ARRAY_SIZE(path), L"%s\\Packages\\%s\\PersistentLocalStorage", base, family_w );
    free( family_w );

    /* create each level */
    for (i = 0; path[i]; i++)
    {
        if (path[i] != '\\' || i < 3) continue;
        path[i] = 0;
        CreateDirectoryW( path, NULL );
        path[i] = '\\';
    }
    CreateDirectoryW( path, NULL );

    n = WideCharToMultiByte( CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL );
    if (n && (ret = malloc( n ))) WideCharToMultiByte( CP_UTF8, 0, path, -1, ret, n, NULL, NULL );
    return ret;
}

static inline struct x_persistent_local_storage *impl_from_IXPersistentLocalStorageImpl3( IXPersistentLocalStorageImpl3 *iface )
{
    return CONTAINING_RECORD( iface, struct x_persistent_local_storage, IXPersistentLocalStorageImpl3_iface );
}

static HRESULT WINAPI x_persistent_local_storage_QueryInterface( IXPersistentLocalStorageImpl3 *iface, REFIID iid, void **out )
{
    struct x_persistent_local_storage *impl = impl_from_IXPersistentLocalStorageImpl3( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown                      ) ||
        IsEqualGUID( iid, &IID_IXPersistentLocalStorageImpl  ) ||
        IsEqualGUID( iid, &IID_IXPersistentLocalStorageImpl2 ) ||
        IsEqualGUID( iid, &IID_IXPersistentLocalStorageImpl3 ))
    {
        IXPersistentLocalStorageImpl_AddRef( *out = &impl->IXPersistentLocalStorageImpl3_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_persistent_local_storage_AddRef( IXPersistentLocalStorageImpl3 *iface )
{
    struct x_persistent_local_storage *impl = impl_from_IXPersistentLocalStorageImpl3( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_persistent_local_storage_Release( IXPersistentLocalStorageImpl3 *iface )
{
    struct x_persistent_local_storage *impl = impl_from_IXPersistentLocalStorageImpl3( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_persistent_local_storage_XPersistentLocalStorageGetPath( IXPersistentLocalStorageImpl3 *iface, SIZE_T pathSize, char *path, SIZE_T *pathUsed )
{
    char *dir;
    SIZE_T len;
    TRACE( "iface %p, pathSize %Iu, path %p.\n", iface, pathSize, path );
    if (!path) return E_INVALIDARG;
    if (!(dir = pls_dir_utf8())) return E_FAIL;
    len = strlen( dir ) + 1;
    if (pathSize < len) { free( dir ); return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER ); }
    memcpy( path, dir, len );
    if (pathUsed) *pathUsed = len;
    free( dir );
    return S_OK;
}

static HRESULT WINAPI x_persistent_local_storage_XPersistentLocalStorageGetPathSize( IXPersistentLocalStorageImpl3 *iface, SIZE_T *pathSize )
{
    char *dir;
    TRACE( "iface %p, pathSize %p.\n", iface, pathSize );
    if (!pathSize) return E_INVALIDARG;
    if (!(dir = pls_dir_utf8())) return E_FAIL;
    *pathSize = strlen( dir ) + 1;
    free( dir );
    return S_OK;
}

static HRESULT WINAPI x_persistent_local_storage_XPersistentLocalStorageGetSpaceInfo( IXPersistentLocalStorageImpl3 *iface, XPersistentLocalStorageSpaceInfo *info )
{
    TRACE( "iface %p, info %p.\n", iface, info );
    if (!info) return E_INVALIDARG;
    /* report ample space so nothing bails on a storage check */
    info->totalBytes = 256ull << 30;
    info->usedBytes = 0;
    info->totalFreeBytes = info->totalBytes;
    info->availableFreeBytes = info->totalBytes;
    return S_OK;
}

static HRESULT WINAPI x_persistent_local_storage_XPersistentLocalStorageMountForPackage( IXPersistentLocalStorageImpl3 *iface, const char *packageIdentifier, XPackageMountHandle *mountHandle )
{
    char *dir;
    TRACE( "iface %p, packageIdentifier %s, mountHandle %p.\n", iface, debugstr_a( packageIdentifier ), mountHandle );
    if (!mountHandle) return E_INVALIDARG;
    if (!(dir = pls_dir_utf8())) return E_FAIL;
    *mountHandle = xgr_make_mount( dir );
    free( dir );
    return *mountHandle ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI x_persistent_local_storage_XPersistentLocalStoragePromptUserForSpaceAsync( IXPersistentLocalStorageImpl3 *iface, UINT64 requestedBytes, XAsyncBlock *asyncBlock )
{
    TRACE( "iface %p, requestedBytes %I64u, asyncBlock %p.\n", iface, requestedBytes, asyncBlock );
    if (!asyncBlock) return E_INVALIDARG;
    xgr_async_complete( asyncBlock, S_OK, NULL ); /* space is always granted */
    return S_OK;
}

static HRESULT WINAPI x_persistent_local_storage_XPersistentLocalStoragePromptUserForSpaceResult( IXPersistentLocalStorageImpl3 *iface, XAsyncBlock *asyncBlock )
{
    HRESULT hr;
    TRACE( "iface %p, asyncBlock %p.\n", iface, asyncBlock );
    if (!xgr_async_completed( asyncBlock, &hr, NULL )) return E_PENDING;
    return hr;
}

static const struct IXPersistentLocalStorageImpl3Vtbl x_persistent_local_storage_vtbl =
{
    x_persistent_local_storage_QueryInterface,
    x_persistent_local_storage_AddRef,
    x_persistent_local_storage_Release,
    /* IXPersistentLocalStorageImpl/IXPersistentLocalStorageImpl2 methods */
    x_persistent_local_storage_XPersistentLocalStorageGetPathSize,
    x_persistent_local_storage_XPersistentLocalStorageGetPath,
    x_persistent_local_storage_XPersistentLocalStorageGetSpaceInfo,
    x_persistent_local_storage_XPersistentLocalStoragePromptUserForSpaceAsync,
    x_persistent_local_storage_XPersistentLocalStoragePromptUserForSpaceResult,
    /* IXPersistentLocalStorageImpl3 methods */
    x_persistent_local_storage_XPersistentLocalStorageMountForPackage,
};

static struct x_persistent_local_storage x_persistent_local_storage =
{
    {&x_persistent_local_storage_vtbl},
    0,
};

IXPersistentLocalStorageImpl *x_persistent_local_storage_impl = (IXPersistentLocalStorageImpl *)&x_persistent_local_storage.IXPersistentLocalStorageImpl3_iface;
