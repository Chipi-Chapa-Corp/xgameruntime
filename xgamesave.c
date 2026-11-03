/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XGameSave and XGameSaveFiles
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

/*
 * Connected storage, kept in the same on-disk layout the Windows Gaming
 * Services use, so a save folder copied over from a Windows install loads
 * as-is and anything written here can be copied back.
 *
 *   <root>\<XUID>_<SCID>\containers.index          list of containers
 *   <root>\<XUID>_<SCID>\<FOLDER>\container.<N>    blob manifest of one container
 *   <root>\<XUID>_<SCID>\<FOLDER>\<FILE>           one blob
 *
 * <root> is %LOCALAPPDATA%\Packages\<PackageFamilyName>\SystemAppData\wgs,
 * with the family name derived from MicrosoftGame.config the way Windows does
 * it; XGR_WGS_ROOT overrides it. <FOLDER> and <FILE> are GUIDs written as
 * 32 uppercase hex digits. Strings in the index are length-prefixed UTF-16,
 * times are FILETIMEs. The manifest is a fixed-size table of blob names.
 *
 * Every operation re-reads the index, so the file on disk is the only state
 * that matters and several providers can coexist in one process.
 */

#include "private.h"

#include <objbase.h>
#include <bcrypt.h>

#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define WGS_INDEX_VERSION       14
#define WGS_MANIFEST_VERSION    4
#define WGS_BLOB_NAME_CHARS     64
#define WGS_DEFAULT_QUOTA       (256ull << 20)
#define GS_MAX_UPDATE_SIZE      (16ull << 20)
#define TICKS_1601_TO_0001      504911232000000000ull
#define TICKS_1601_TO_1970      116444736000000000ull

struct wgs_blob
{
    struct list entry;
    WCHAR name[WGS_BLOB_NAME_CHARS];
    GUID file;
    GUID file_alt;
    UINT64 size;
};

struct wgs_container
{
    struct list entry;
    WCHAR *name;
    WCHAR *display_name;
    WCHAR *etag;
    BYTE number;        /* the N of container.<N> */
    UINT32 flags;
    GUID folder;
    UINT64 mtime;
    UINT64 reserved;
    UINT64 size;
};

struct wgs_index
{
    UINT32 version;
    UINT32 reserved0;
    WCHAR *aumid;
    UINT64 mtime;
    UINT32 reserved1;
    WCHAR *id;
    UINT64 quota;
    struct list containers;
};

struct XGameSaveProvider
{
    LONG ref;
    WCHAR *dir;
    BOOL read_only;         /* set when the index could not be parsed */
    struct wgs_index index;
};

struct x_string_entry
{
    struct list entry;
    char str[1];
};

struct XGameSaveContainer
{
    struct XGameSaveProvider *provider;
    WCHAR *name;
    struct list strings;    /* blob names handed out to the title */
};

struct wgs_op
{
    struct list entry;
    BOOL is_delete;
    WCHAR name[WGS_BLOB_NAME_CHARS];
    GUID file;
    BYTE *data;
    SIZE_T size;
};

struct XGameSaveUpdate
{
    struct XGameSaveContainer *container;
    WCHAR *display_name;
    struct list ops;
    SIZE_T total_size;
};

struct read_request
{
    struct XGameSaveContainer *container;
    UINT32 count;
    char **names;
};

struct old_file
{
    struct list entry;
    GUID file;
};

static CRITICAL_SECTION gs_cs;
static CRITICAL_SECTION_DEBUG gs_cs_debug =
{
    0, 0, &gs_cs,
    { &gs_cs_debug.ProcessLocksList, &gs_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": gs_cs") }
};
static CRITICAL_SECTION gs_cs = { &gs_cs_debug, -1, 0, 0, 0, 0 };

/* ---- strings ------------------------------------------------------------ */

static WCHAR *utf8_to_wide( const char *str )
{
    WCHAR *ret;
    int len;

    if (!str) return NULL;
    if (!(len = MultiByteToWideChar( CP_UTF8, 0, str, -1, NULL, 0 ))) return NULL;
    if (!(ret = malloc( len * sizeof(WCHAR) ))) return NULL;
    MultiByteToWideChar( CP_UTF8, 0, str, -1, ret, len );
    return ret;
}

static char *wide_to_utf8( const WCHAR *str )
{
    char *ret;
    int len;

    if (!str) return NULL;
    if (!(len = WideCharToMultiByte( CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL ))) return NULL;
    if (!(ret = malloc( len ))) return NULL;
    WideCharToMultiByte( CP_UTF8, 0, str, -1, ret, len, NULL, NULL );
    return ret;
}

static WCHAR *wide_dup( const WCHAR *str )
{
    WCHAR *ret;
    SIZE_T size;

    if (!str) return NULL;
    size = (wcslen( str ) + 1) * sizeof(WCHAR);
    if ((ret = malloc( size ))) memcpy( ret, str, size );
    return ret;
}

static char *narrow_dup( const char *str )
{
    char *ret;
    SIZE_T size;

    if (!str) return NULL;
    size = strlen( str ) + 1;
    if ((ret = malloc( size ))) memcpy( ret, str, size );
    return ret;
}

static WCHAR *path_join( const WCHAR *dir, const WCHAR *leaf )
{
    WCHAR *ret;
    SIZE_T len = wcslen( dir ) + 1 + wcslen( leaf ) + 1;

    if (!(ret = malloc( len * sizeof(WCHAR) ))) return NULL;
    swprintf( ret, len, L"%s\\%s", dir, leaf );
    return ret;
}

/* 32 uppercase hex digits, the way the folder and blob files are named */
static void guid_to_name( const GUID *guid, WCHAR name[33] )
{
    swprintf( name, 33, L"%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
              (unsigned int)guid->Data1, guid->Data2, guid->Data3,
              guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
              guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7] );
}

static void guid_to_string( const GUID *guid, WCHAR str[37] )
{
    swprintf( str, 37, L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
              (unsigned int)guid->Data1, guid->Data2, guid->Data3,
              guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
              guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7] );
}

static UINT64 filetime_now( void )
{
    FILETIME ft;
    GetSystemTimeAsFileTime( &ft );
    return ((UINT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/* ---- files -------------------------------------------------------------- */

static BOOL read_whole_file( const WCHAR *path, BYTE **data, SIZE_T *size )
{
    HANDLE file;
    LARGE_INTEGER len;
    DWORD read, total = 0;
    BYTE *buf;

    *data = NULL;
    *size = 0;
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!GetFileSizeEx( file, &len ) || len.QuadPart > 0x7fffffff || !(buf = malloc( len.QuadPart + 1 )))
    {
        CloseHandle( file );
        return FALSE;
    }
    while (total < len.QuadPart)
    {
        if (!ReadFile( file, buf + total, len.QuadPart - total, &read, NULL ) || !read) break;
        total += read;
    }
    CloseHandle( file );
    if (total != len.QuadPart)
    {
        free( buf );
        return FALSE;
    }
    buf[total] = 0;
    *data = buf;
    *size = total;
    return TRUE;
}

static BOOL write_whole_file( const WCHAR *path, const BYTE *data, SIZE_T size )
{
    HANDLE file;
    DWORD written;
    SIZE_T total = 0;
    BOOL ok = TRUE;

    file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    while (ok && total < size)
    {
        ok = WriteFile( file, data + total, size - total, &written, NULL );
        total += written;
    }
    if (ok) ok = FlushFileBuffers( file );
    CloseHandle( file );
    if (!ok) DeleteFileW( path );
    return ok;
}

/* Replace a file without ever leaving a half-written one in its place. */
static BOOL write_file_atomic( const WCHAR *path, const BYTE *data, SIZE_T size )
{
    WCHAR *tmp;
    SIZE_T len = wcslen( path ) + 5;
    BOOL ok;

    if (!(tmp = malloc( len * sizeof(WCHAR) ))) return FALSE;
    swprintf( tmp, len, L"%s.tmp", path );
    ok = write_whole_file( tmp, data, size ) && MoveFileExW( tmp, path, MOVEFILE_REPLACE_EXISTING );
    if (!ok) DeleteFileW( tmp );
    free( tmp );
    return ok;
}

static BOOL file_size( const WCHAR *path, UINT64 *size )
{
    WIN32_FILE_ATTRIBUTE_DATA info;

    if (!GetFileAttributesExW( path, GetFileExInfoStandard, &info )) return FALSE;
    *size = ((UINT64)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    return TRUE;
}

static BOOL make_directory( const WCHAR *path )
{
    WCHAR *copy, *p, *start;
    BOOL ok = TRUE;

    if (!(copy = wide_dup( path ))) return FALSE;
    start = copy;
    if (start[0] && start[1] == ':') start += 2;
    while (*start == '\\') start++;
    for (p = start; ok && *p; p++)
    {
        if (*p != '\\') continue;
        *p = 0;
        if (!CreateDirectoryW( copy, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS) ok = FALSE;
        *p = '\\';
    }
    if (ok && !CreateDirectoryW( copy, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS) ok = FALSE;
    free( copy );
    return ok;
}

/* ---- binary reader / writer --------------------------------------------- */

struct reader
{
    const BYTE *p;
    const BYTE *end;
    BOOL ok;
};

static BOOL rd_bytes( struct reader *r, void *out, SIZE_T len )
{
    if (!r->ok || (SIZE_T)(r->end - r->p) < len)
    {
        r->ok = FALSE;
        return FALSE;
    }
    memcpy( out, r->p, len );
    r->p += len;
    return TRUE;
}

static UINT32 rd_u32( struct reader *r )
{
    UINT32 v = 0;
    rd_bytes( r, &v, sizeof(v) );
    return v;
}

static UINT64 rd_u64( struct reader *r )
{
    UINT64 v = 0;
    rd_bytes( r, &v, sizeof(v) );
    return v;
}

static BYTE rd_u8( struct reader *r )
{
    BYTE v = 0;
    rd_bytes( r, &v, sizeof(v) );
    return v;
}

static WCHAR *rd_wstr( struct reader *r )
{
    UINT32 len = rd_u32( r );
    WCHAR *str;

    if (!r->ok || len > 0x10000 || (SIZE_T)(r->end - r->p) < len * sizeof(WCHAR) ||
        !(str = malloc( (len + 1) * sizeof(WCHAR) )))
    {
        r->ok = FALSE;
        return NULL;
    }
    memcpy( str, r->p, len * sizeof(WCHAR) );
    str[len] = 0;
    r->p += len * sizeof(WCHAR);
    return str;
}

struct writer
{
    BYTE *buf;
    SIZE_T len;
    SIZE_T cap;
    BOOL ok;
};

static void wr_bytes( struct writer *w, const void *data, SIZE_T len )
{
    if (!w->ok) return;
    if (w->len + len > w->cap)
    {
        SIZE_T cap = w->cap * 2 > w->len + len + 1024 ? w->cap * 2 : w->len + len + 1024;
        BYTE *buf = realloc( w->buf, cap );
        if (!buf)
        {
            w->ok = FALSE;
            return;
        }
        w->buf = buf;
        w->cap = cap;
    }
    memcpy( w->buf + w->len, data, len );
    w->len += len;
}

static void wr_u32( struct writer *w, UINT32 v ) { wr_bytes( w, &v, sizeof(v) ); }
static void wr_u64( struct writer *w, UINT64 v ) { wr_bytes( w, &v, sizeof(v) ); }
static void wr_u8( struct writer *w, BYTE v ) { wr_bytes( w, &v, sizeof(v) ); }

static void wr_wstr( struct writer *w, const WCHAR *str )
{
    UINT32 len = str ? wcslen( str ) : 0;
    wr_u32( w, len );
    if (len) wr_bytes( w, str, len * sizeof(WCHAR) );
}

/* ---- package identity ---------------------------------------------------- */

static BYTE *load_game_config( void )
{
    WCHAR exe[MAX_PATH], *path;
    BYTE *data;
    SIZE_T size;
    int i;

    if (xgr_game_config && strstr( xgr_game_config, "<Identity" )) return (BYTE *)narrow_dup( xgr_game_config );

    if (GetCurrentDirectoryW( ARRAY_SIZE(exe), exe ) && (path = path_join( exe, L"MicrosoftGame.config" )))
    {
        BOOL ok = read_whole_file( path, &data, &size );
        free( path );
        if (ok) return data;
    }
    if (GetModuleFileNameW( NULL, exe, ARRAY_SIZE(exe) ))
    {
        for (i = 0; i < 6; i++)
        {
            WCHAR *sep = wcsrchr( exe, '\\' );
            BOOL ok;

            if (!sep) break;
            *sep = 0;
            if (!(path = path_join( exe, L"MicrosoftGame.config" ))) break;
            ok = read_whole_file( path, &data, &size );
            free( path );
            if (ok) return data;
        }
    }
    return NULL;
}

/* Pull attr="value" out of the first <element ...> in the document. */
static BOOL xml_attr( const char *xml, const char *element, const char *attr, char *out, SIZE_T cap )
{
    const char *start, *end, *value;
    SIZE_T len;

    if (!(start = strstr( xml, element )) || !(end = strchr( start, '>' ))) return FALSE;
    if (!(value = strstr( start, attr )) || value > end) return FALSE;
    value += strlen( attr );
    len = strcspn( value, "\"" );
    if (len >= cap) return FALSE;
    memcpy( out, value, len );
    out[len] = 0;
    return TRUE;
}

/* The 13-character suffix of a package family name: Crockford base32 of the
 * first 8 bytes of SHA-256 over the UTF-16 publisher string. */
static BOOL publisher_hash( const char *publisher, WCHAR out[14] )
{
    static const char alphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";
    BCRYPT_ALG_HANDLE alg;
    UCHAR hash[32];
    WCHAR *wide;
    UINT64 v = 0;
    NTSTATUS status;
    int i;

    if (!(wide = utf8_to_wide( publisher ))) return FALSE;
    status = BCryptOpenAlgorithmProvider( &alg, BCRYPT_SHA256_ALGORITHM, NULL, 0 );
    if (!status)
    {
        status = BCryptHash( alg, NULL, 0, (UCHAR *)wide, wcslen( wide ) * sizeof(WCHAR), hash, sizeof(hash) );
        BCryptCloseAlgorithmProvider( alg, 0 );
    }
    free( wide );
    if (status) return FALSE;

    for (i = 0; i < 8; i++) v = (v << 8) | hash[i];
    for (i = 0; i < 12; i++) out[i] = alphabet[(v >> (59 - 5 * i)) & 31];
    out[12] = alphabet[(v & 15) << 1];
    out[13] = 0;
    return TRUE;
}

static WCHAR *package_family;   /* <Name>_<hash>, NULL when unknown */
static WCHAR *package_aumid;    /* <family>!<executable id> */
static BOOL package_identity_done;

/* Work out the package family name from MicrosoftGame.config, once. */
static void resolve_package_identity( void )
{
    BYTE *xml;
    char name[128], publisher[512], app[128];
    WCHAR hash[14], *name_w, buf[320];

    if (package_identity_done) return;
    package_identity_done = TRUE;

    if (!(xml = load_game_config()))
    {
        WARN( "MicrosoftGame.config not found, package identity unknown.\n" );
        return;
    }
    if (xml_attr( (char *)xml, "<Identity", " Name=\"", name, sizeof(name) ) &&
        xml_attr( (char *)xml, "<Identity", " Publisher=\"", publisher, sizeof(publisher) ) &&
        publisher_hash( publisher, hash ) && (name_w = utf8_to_wide( name )))
    {
        swprintf( buf, ARRAY_SIZE(buf), L"%s_%s", name_w, hash );
        free( name_w );
        package_family = wide_dup( buf );

        if (!xml_attr( (char *)xml, "<Executable ", " Id=\"", app, sizeof(app) )) strcpy( app, "Game" );
        if ((name_w = utf8_to_wide( app )))
        {
            swprintf( buf, ARRAY_SIZE(buf), L"%s!%s", package_family, name_w );
            free( name_w );
            package_aumid = wide_dup( buf );
        }
        TRACE( "package family %s, aumid %s.\n", debugstr_w( package_family ), debugstr_w( package_aumid ) );
    }
    else WARN( "could not read package identity from MicrosoftGame.config.\n" );
    free( xml );
}

/* %LOCALAPPDATA%\Packages\<family>\SystemAppData\wgs, unless overridden */
static WCHAR *resolve_root( void )
{
    WCHAR buf[MAX_PATH], *base, *packages, *root = NULL;
    DWORD len;

    if ((len = GetEnvironmentVariableW( L"XGR_WGS_ROOT", buf, ARRAY_SIZE(buf) )) && len < ARRAY_SIZE(buf))
        return wide_dup( buf );

    if ((len = GetEnvironmentVariableW( L"LOCALAPPDATA", buf, ARRAY_SIZE(buf) )) && len < ARRAY_SIZE(buf))
        base = wide_dup( buf );
    else if ((len = GetEnvironmentVariableW( L"USERPROFILE", buf, ARRAY_SIZE(buf) )) && len < ARRAY_SIZE(buf))
        base = path_join( buf, L"AppData\\Local" );
    else
        return NULL;
    if (!base) return NULL;

    resolve_package_identity();
    if ((packages = path_join( base, L"Packages" )))
    {
        WCHAR *pkg = path_join( packages, package_family ? package_family : L"UnknownPackage" );
        if (pkg) root = path_join( pkg, L"SystemAppData\\wgs" );
        free( pkg );
        free( packages );
    }
    free( base );
    return root;
}

/* "00000000-0000-0000-0000-0000697f9ec3" -> "000000000000000000000000697F9EC3" */
static BOOL scid_to_suffix( const char *scid, WCHAR out[33] )
{
    int n = 0;

    for (; *scid && n < 32; scid++)
    {
        char c = *scid;
        if (c == '-' || c == '{' || c == '}') continue;
        if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return FALSE;
        out[n++] = c;
    }
    out[n] = 0;
    return n == 32 && !*scid;
}

/* The per-user folder for a service configuration. An existing folder with
 * the right SCID suffix wins whatever XUID it carries, which is how a save
 * copied over from Windows is picked up; otherwise one is named after the
 * local user. */
static WCHAR *resolve_user_dir( const WCHAR *root, const char *scid )
{
    WCHAR suffix[33], leaf[80], *pattern, *dir = NULL;
    WIN32_FIND_DATAW data;
    HANDLE find;

    if (!scid_to_suffix( scid, suffix )) return NULL;
    swprintf( leaf, ARRAY_SIZE(leaf), L"*_%s", suffix );
    if (!(pattern = path_join( root, leaf ))) return NULL;
    find = FindFirstFileW( pattern, &data );
    free( pattern );
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcslen( data.cFileName ) == 16 + 1 + 32)
            {
                dir = path_join( root, data.cFileName );
                break;
            }
        } while (FindNextFileW( find, &data ));
        FindClose( find );
    }
    if (!dir)
    {
        swprintf( leaf, ARRAY_SIZE(leaf), L"%016I64X_%s", xgr_local_xuid(), suffix );
        dir = path_join( root, leaf );
    }
    return dir;
}

/* ---- shared package identity (used by XPackage too) --------------------- */

/* MicrosoftGame.config text, from the inline copy or from disk next to the
 * executable (NUL-terminated, caller frees). Shared with XPackage/XStore. */
char *xgr_load_game_config( void )
{
    return (char *)load_game_config();
}

/* The derived package family name (<Identity Name>_<hash>) as UTF-8, or NULL.
 * Single source of truth: the same value the save-folder layout is keyed on. */
char *xgr_package_family( void )
{
    resolve_package_identity();
    return package_family ? wide_to_utf8( package_family ) : NULL;
}

/* The install root: the directory that holds MicrosoftGame.config, which is
 * what XPackage reports as the current package's mount path. The executable
 * lives several directories below it (Sandfall\Binaries\WinGDK\... for a UE5
 * title), so walk up until the config is found; fall back to the CWD. */
WCHAR *xgr_package_root( void )
{
    WCHAR exe[MAX_PATH], *path;
    int i;

    if (GetModuleFileNameW( NULL, exe, ARRAY_SIZE(exe) ))
    {
        for (i = 0; i < 8; i++)
        {
            WCHAR *sep = wcsrchr( exe, '\\' );
            DWORD attr;

            if (!sep) break;
            *sep = 0;
            if (!(path = path_join( exe, L"MicrosoftGame.config" ))) break;
            attr = GetFileAttributesW( path );
            free( path );
            if (attr != INVALID_FILE_ATTRIBUTES) return wide_dup( exe );
        }
    }
    if (GetCurrentDirectoryW( ARRAY_SIZE(exe), exe )) return wide_dup( exe );
    return NULL;
}

/* ---- index -------------------------------------------------------------- */

static void free_container( struct wgs_container *c )
{
    free( c->name );
    free( c->display_name );
    free( c->etag );
    free( c );
}

static void index_free( struct wgs_index *idx )
{
    struct wgs_container *c, *next;

    if (idx->containers.next)
    {
        LIST_FOR_EACH_ENTRY_SAFE( c, next, &idx->containers, struct wgs_container, entry )
        {
            list_remove( &c->entry );
            free_container( c );
        }
    }
    free( idx->aumid );
    free( idx->id );
    memset( idx, 0, sizeof(*idx) );
    list_init( &idx->containers );
}

static void index_init_new( struct wgs_index *idx )
{
    GUID guid;
    WCHAR buf[37];

    idx->version = WGS_INDEX_VERSION;
    idx->mtime = filetime_now();
    idx->reserved1 = 3;
    CoCreateGuid( &guid );
    guid_to_string( &guid, buf );
    idx->id = wide_dup( buf );
    idx->quota = WGS_DEFAULT_QUOTA;
}

/* S_OK: parsed; S_FALSE: no index yet (a fresh one is set up); error: corrupt. */
static HRESULT index_read( struct wgs_index *idx, const WCHAR *path )
{
    BYTE *data;
    SIZE_T size;
    struct reader r;
    UINT32 count, i;

    index_free( idx );
    if (!read_whole_file( path, &data, &size ))
    {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
        {
            ERR( "cannot read %s, error %lu.\n", debugstr_w( path ), err );
            return E_GS_NO_ACCESS;
        }
        index_init_new( idx );
        return S_FALSE;
    }

    r.p = data;
    r.end = data + size;
    r.ok = TRUE;
    idx->version = rd_u32( &r );
    count = rd_u32( &r );
    idx->reserved0 = rd_u32( &r );
    idx->aumid = rd_wstr( &r );
    idx->mtime = rd_u64( &r );
    idx->reserved1 = rd_u32( &r );
    idx->id = rd_wstr( &r );
    idx->quota = rd_u64( &r );
    for (i = 0; r.ok && i < count; i++)
    {
        struct wgs_container *c;

        if (!(c = calloc( 1, sizeof(*c) )))
        {
            r.ok = FALSE;
            break;
        }
        c->name = rd_wstr( &r );
        c->display_name = rd_wstr( &r );
        c->etag = rd_wstr( &r );
        c->number = rd_u8( &r );
        c->flags = rd_u32( &r );
        rd_bytes( &r, &c->folder, sizeof(c->folder) );
        c->mtime = rd_u64( &r );
        c->reserved = rd_u64( &r );
        c->size = rd_u64( &r );
        list_add_tail( &idx->containers, &c->entry );
    }
    free( data );

    if (!r.ok || idx->version != WGS_INDEX_VERSION)
    {
        ERR( "%s: unrecognised index, version %u, %u containers, %s.\n", debugstr_w( path ),
             idx->version, count, r.ok ? "parsed" : "truncated" );
        index_free( idx );
        return E_GS_CONTAINER_SYNC_FAILED;
    }
    if (!idx->quota) idx->quota = WGS_DEFAULT_QUOTA;
    TRACE( "%s: %u containers, quota %I64u, aumid %s.\n", debugstr_w( path ), count, idx->quota, debugstr_w( idx->aumid ) );
    return S_OK;
}

static BOOL index_write( struct wgs_index *idx, const WCHAR *path )
{
    struct writer w = { 0 };
    struct wgs_container *c;
    UINT32 count = 0;
    BOOL ok;

    w.ok = TRUE;
    LIST_FOR_EACH_ENTRY( c, &idx->containers, struct wgs_container, entry ) count++;
    idx->mtime = filetime_now();

    wr_u32( &w, idx->version );
    wr_u32( &w, count );
    wr_u32( &w, idx->reserved0 );
    wr_wstr( &w, idx->aumid );
    wr_u64( &w, idx->mtime );
    wr_u32( &w, idx->reserved1 );
    wr_wstr( &w, idx->id );
    wr_u64( &w, idx->quota );
    LIST_FOR_EACH_ENTRY( c, &idx->containers, struct wgs_container, entry )
    {
        wr_wstr( &w, c->name );
        wr_wstr( &w, c->display_name );
        wr_wstr( &w, c->etag );
        wr_u8( &w, c->number );
        wr_u32( &w, c->flags );
        wr_bytes( &w, &c->folder, sizeof(c->folder) );
        wr_u64( &w, c->mtime );
        wr_u64( &w, c->reserved );
        wr_u64( &w, c->size );
    }
    ok = w.ok && write_file_atomic( path, w.buf, w.len );
    free( w.buf );
    return ok;
}

/* ---- container manifest ------------------------------------------------- */

static void blobs_free( struct list *blobs )
{
    struct wgs_blob *b, *next;

    LIST_FOR_EACH_ENTRY_SAFE( b, next, blobs, struct wgs_blob, entry )
    {
        list_remove( &b->entry );
        free( b );
    }
}

static struct wgs_blob *blobs_find( struct list *blobs, const WCHAR *name )
{
    struct wgs_blob *b;

    LIST_FOR_EACH_ENTRY( b, blobs, struct wgs_blob, entry )
        if (!_wcsicmp( b->name, name )) return b;
    return NULL;
}

static WCHAR *container_dir( const struct XGameSaveProvider *p, const struct wgs_container *c )
{
    WCHAR name[33];
    guid_to_name( &c->folder, name );
    return path_join( p->dir, name );
}

static WCHAR *manifest_path( const WCHAR *dir, BYTE number )
{
    WCHAR leaf[32];
    swprintf( leaf, ARRAY_SIZE(leaf), L"container.%u", number );
    return path_join( dir, leaf );
}

static WCHAR *blob_path( const WCHAR *dir, const GUID *file )
{
    WCHAR name[33];
    guid_to_name( file, name );
    return path_join( dir, name );
}

/* S_OK: parsed; S_FALSE: no manifest (empty list); error: corrupt. Sizes come
 * from the blob files themselves, the manifest only maps names to them. */
static HRESULT manifest_read( const WCHAR *dir, const struct wgs_container *c, struct list *blobs )
{
    WCHAR *path;
    BYTE *data;
    SIZE_T size;
    struct reader r;
    UINT32 version, count, i;

    list_init( blobs );
    if (!(path = manifest_path( dir, c->number ))) return E_OUTOFMEMORY;
    if (!read_whole_file( path, &data, &size ))
    {
        free( path );
        return S_FALSE;
    }

    r.p = data;
    r.end = data + size;
    r.ok = TRUE;
    version = rd_u32( &r );
    count = rd_u32( &r );
    for (i = 0; r.ok && i < count; i++)
    {
        struct wgs_blob *b;
        WCHAR *file;

        if (!(b = calloc( 1, sizeof(*b) )))
        {
            r.ok = FALSE;
            break;
        }
        rd_bytes( &r, b->name, sizeof(b->name) );
        b->name[WGS_BLOB_NAME_CHARS - 1] = 0;
        rd_bytes( &r, &b->file, sizeof(b->file) );
        rd_bytes( &r, &b->file_alt, sizeof(b->file_alt) );
        if ((file = blob_path( dir, &b->file )))
        {
            if (!file_size( file, &b->size )) b->size = 0;
            free( file );
        }
        list_add_tail( blobs, &b->entry );
    }
    free( data );

    if (!r.ok || version != WGS_MANIFEST_VERSION)
    {
        ERR( "%s: unrecognised manifest, version %u, %u blobs, %s.\n", debugstr_w( path ),
             version, count, r.ok ? "parsed" : "truncated" );
        free( path );
        blobs_free( blobs );
        return E_GS_CONTAINER_SYNC_FAILED;
    }
    free( path );
    return S_OK;
}

static BOOL manifest_write( const WCHAR *dir, BYTE number, struct list *blobs )
{
    struct writer w = { 0 };
    struct wgs_blob *b;
    WCHAR *path;
    UINT32 count = 0;
    BOOL ok;

    w.ok = TRUE;
    LIST_FOR_EACH_ENTRY( b, blobs, struct wgs_blob, entry ) count++;
    wr_u32( &w, WGS_MANIFEST_VERSION );
    wr_u32( &w, count );
    LIST_FOR_EACH_ENTRY( b, blobs, struct wgs_blob, entry )
    {
        wr_bytes( &w, b->name, sizeof(b->name) );
        wr_bytes( &w, &b->file, sizeof(b->file) );
        wr_bytes( &w, &b->file_alt, sizeof(b->file_alt) );
    }
    if (!(path = manifest_path( dir, number )))
    {
        free( w.buf );
        return FALSE;
    }
    ok = w.ok && write_file_atomic( path, w.buf, w.len );
    free( path );
    free( w.buf );
    return ok;
}

/* ---- provider ----------------------------------------------------------- */

static void provider_addref( struct XGameSaveProvider *p )
{
    InterlockedIncrement( &p->ref );
}

static void provider_release( struct XGameSaveProvider *p )
{
    if (InterlockedDecrement( &p->ref )) return;
    index_free( &p->index );
    free( p->dir );
    free( p );
}

/* Refresh from disk. Called with gs_cs held. */
static HRESULT provider_reload( struct XGameSaveProvider *p )
{
    WCHAR *path;
    HRESULT hr;

    if (!(path = path_join( p->dir, L"containers.index" ))) return E_OUTOFMEMORY;
    hr = index_read( &p->index, path );
    free( path );
    p->read_only = FAILED( hr );
    return FAILED( hr ) ? hr : S_OK;
}

/* Write the index back. Called with gs_cs held. */
static HRESULT provider_commit( struct XGameSaveProvider *p )
{
    WCHAR *path;
    BOOL ok;

    if (p->read_only) return E_GS_NO_ACCESS;
    if (!p->index.aumid)
    {
        resolve_package_identity();
        p->index.aumid = wide_dup( package_aumid ? package_aumid : L"" );
    }
    if (!make_directory( p->dir )) return E_GS_OUT_OF_LOCAL_STORAGE;
    if (!(path = path_join( p->dir, L"containers.index" ))) return E_OUTOFMEMORY;
    ok = index_write( &p->index, path );
    if (!ok) ERR( "cannot write %s, error %lu.\n", debugstr_w( path ), GetLastError() );
    free( path );
    return ok ? S_OK : E_GS_OUT_OF_LOCAL_STORAGE;
}

static struct wgs_container *provider_find( struct XGameSaveProvider *p, const WCHAR *name )
{
    struct wgs_container *c;

    LIST_FOR_EACH_ENTRY( c, &p->index.containers, struct wgs_container, entry )
        if (!_wcsicmp( c->name, name )) return c;
    return NULL;
}

static UINT64 provider_used( struct XGameSaveProvider *p )
{
    struct wgs_container *c;
    UINT64 used = 0;

    LIST_FOR_EACH_ENTRY( c, &p->index.containers, struct wgs_container, entry ) used += c->size;
    return used;
}

static INT64 provider_remaining( struct XGameSaveProvider *p )
{
    UINT64 used = provider_used( p );
    return used < p->index.quota ? p->index.quota - used : 0;
}

static HRESULT create_provider( XUserHandle user, const char *scid, struct XGameSaveProvider **out )
{
    struct XGameSaveProvider *p;
    WCHAR *root;
    HRESULT hr;

    if (!user || !scid || !out) return E_INVALIDARG;
    if (!(p = calloc( 1, sizeof(*p) ))) return E_OUTOFMEMORY;
    p->ref = 1;
    list_init( &p->index.containers );

    if (!(root = resolve_root()))
    {
        provider_release( p );
        return E_GS_OUT_OF_LOCAL_STORAGE;
    }
    p->dir = resolve_user_dir( root, scid );
    free( root );
    if (!p->dir)
    {
        provider_release( p );
        return E_INVALIDARG;
    }

    EnterCriticalSection( &gs_cs );
    hr = provider_reload( p );
    LeaveCriticalSection( &gs_cs );
    if (FAILED( hr ))
    {
        provider_release( p );
        return hr;
    }
    TRACE( "provider %p for %s at %s.\n", p, debugstr_a( scid ), debugstr_w( p->dir ) );
    *out = p;
    return S_OK;
}

/* Rules from the documentation: the path part may use [A-Za-z0-9_/], the
 * final component [A-Za-z0-9_.-], non-empty, no trailing or doubled dot. */
static BOOL is_name_char( char c, BOOL leaf )
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') return TRUE;
    return leaf ? (c == '.' || c == '-') : c == '/';
}

static BOOL valid_container_name( const char *name )
{
    const char *p, *leaf;

    if (!name || !*name) return FALSE;
    leaf = strrchr( name, '/' );
    leaf = leaf ? leaf + 1 : name;
    for (p = name; p < leaf; p++) if (!is_name_char( *p, FALSE )) return FALSE;
    if (!*leaf || leaf[strlen( leaf ) - 1] == '.' || strstr( leaf, ".." )) return FALSE;
    for (p = leaf; *p; p++) if (!is_name_char( *p, TRUE )) return FALSE;
    return TRUE;
}

static UINT64 filetime_to_unix( UINT64 ft )
{
    return ft > TICKS_1601_TO_1970 ? (ft - TICKS_1601_TO_1970) / 10000000 : 0;
}

/* Hand one container's description to the title. Called with gs_cs held. */
static BOOL report_container( struct XGameSaveProvider *p, struct wgs_container *c, void *context,
                              XGameSaveContainerInfoCallback *callback )
{
    XGameSaveContainerInfo info;
    struct list blobs;
    struct wgs_blob *b;
    WCHAR *dir;
    UINT32 count = 0;
    BOOL cont = TRUE;

    memset( &info, 0, sizeof(info) );
    info.name = wide_to_utf8( c->name );
    info.displayName = wide_to_utf8( c->display_name );
    if ((dir = container_dir( p, c )))
    {
        if (SUCCEEDED( manifest_read( dir, c, &blobs ) ))
        {
            LIST_FOR_EACH_ENTRY( b, &blobs, struct wgs_blob, entry ) count++;
            blobs_free( &blobs );
        }
        free( dir );
    }
    info.blobCount = count;
    info.totalSize = c->size;
    info.lastModifiedTime = filetime_to_unix( c->mtime );
    info.needsSync = FALSE;

    TRACE( "container %s (%s), %u blobs, %I64u bytes.\n", debugstr_a( info.name ), debugstr_a( info.displayName ),
           count, c->size );
    if (info.name && info.displayName) cont = callback( &info, context );
    free( (char *)info.name );
    free( (char *)info.displayName );
    return cont;
}

/* Remember a blob name for as long as the container handle lives, for the
 * case where the title's buffer has no room for the names. */
static const char *container_string( struct XGameSaveContainer *container, const WCHAR *str )
{
    struct x_string_entry *e;
    int len;

    if (!(len = WideCharToMultiByte( CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL ))) return NULL;
    if (!(e = malloc( sizeof(*e) + len ))) return NULL;
    WideCharToMultiByte( CP_UTF8, 0, str, -1, e->str, len, NULL, NULL );
    list_add_tail( &container->strings, &e->entry );
    return e->str;
}

/* The core of XGameSaveReadBlobData. With buffer NULL only the count and the
 * size a buffer needs are reported. Called with gs_cs held.
 *
 * The buffer holds an XGameSaveBlob per blob followed by the data; the names
 * are appended too when there is room, otherwise they live with the handle. */
static HRESULT read_blobs( struct XGameSaveContainer *container, const char **names, UINT32 *count,
                           SIZE_T buffer_size, XGameSaveBlob *buffer, SIZE_T *required )
{
    struct XGameSaveProvider *p = container->provider;
    struct wgs_container *c;
    struct wgs_blob **selected = NULL, *b;
    struct list blobs;
    WCHAR *dir = NULL;
    SIZE_T data_size = 0, names_size = 0, need;
    UINT32 n = 0, total = 0, i;
    BYTE *cursor;
    char *name_cursor;
    HRESULT hr;

    list_init( &blobs );
    if ((hr = provider_reload( p )) != S_OK) return hr;
    if ((c = provider_find( p, container->name )))
    {
        if (!(dir = container_dir( p, c ))) return E_OUTOFMEMORY;
        if (FAILED( hr = manifest_read( dir, c, &blobs ) ))
        {
            free( dir );
            return hr;
        }
    }

    LIST_FOR_EACH_ENTRY( b, &blobs, struct wgs_blob, entry ) total++;
    if (!(selected = calloc( (names ? *count : total) + 1, sizeof(*selected) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (names)
    {
        for (i = 0; i < *count; i++)
        {
            WCHAR *name = utf8_to_wide( names[i] );
            struct wgs_blob *found = name ? blobs_find( &blobs, name ) : NULL;

            free( name );
            if (!found)
            {
                TRACE( "blob %s not found in %s.\n", debugstr_a( names[i] ), debugstr_w( container->name ) );
                hr = E_GS_BLOB_NOT_FOUND;
                goto done;
            }
            selected[n++] = found;
        }
    }
    else
    {
        LIST_FOR_EACH_ENTRY( b, &blobs, struct wgs_blob, entry ) selected[n++] = b;
    }

    for (i = 0; i < n; i++)
    {
        data_size += selected[i]->size;
        names_size += WideCharToMultiByte( CP_UTF8, 0, selected[i]->name, -1, NULL, 0, NULL, NULL );
    }
    need = n * sizeof(XGameSaveBlob) + data_size;
    if (required) *required = need + names_size;
    if (!buffer)
    {
        *count = n;
        hr = S_OK;
        goto done;
    }
    if (buffer_size < need)
    {
        WARN( "buffer %Iu bytes, %u blobs need %Iu.\n", buffer_size, n, need );
        *count = n;
        hr = E_GS_PROVIDED_BUFFER_TOO_SMALL;
        goto done;
    }

    cursor = (BYTE *)buffer + n * sizeof(XGameSaveBlob);
    name_cursor = buffer_size >= need + names_size ? (char *)cursor + data_size : NULL;
    for (i = 0; i < n; i++)
    {
        WCHAR *path;
        BYTE *data;
        SIZE_T size;
        BOOL ok;

        b = selected[i];
        if (!(path = blob_path( dir, &b->file )))
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        ok = read_whole_file( path, &data, &size );
        free( path );
        if (!ok)
        {
            ERR( "blob %s of %s has no file.\n", debugstr_w( b->name ), debugstr_w( container->name ) );
            hr = E_GS_BLOB_NOT_FOUND;
            goto done;
        }
        if (size > b->size) size = b->size;
        memcpy( cursor, data, size );
        if (size < b->size) memset( cursor + size, 0, b->size - size );
        free( data );

        buffer[i].data = cursor;
        buffer[i].info.size = b->size;
        cursor += b->size;
        if (name_cursor)
        {
            int len = WideCharToMultiByte( CP_UTF8, 0, b->name, -1, name_cursor, names_size, NULL, NULL );
            buffer[i].info.name = name_cursor;
            name_cursor += len;
            names_size -= len;
        }
        else buffer[i].info.name = container_string( container, b->name );
        TRACE( "read blob %s, %u bytes.\n", debugstr_a( buffer[i].info.name ), buffer[i].info.size );
    }
    *count = n;
    hr = S_OK;

done:
    free( selected );
    blobs_free( &blobs );
    free( dir );
    return hr;
}

static void free_read_request( struct read_request *req )
{
    UINT32 i;

    if (!req) return;
    if (req->names) for (i = 0; i < req->count; i++) free( req->names[i] );
    free( req->names );
    free( req );
}

/* Apply a queued update to disk. Called with gs_cs held. */
static HRESULT submit_update( struct XGameSaveUpdate *update )
{
    struct XGameSaveContainer *container = update->container;
    struct XGameSaveProvider *p = container->provider;
    struct wgs_container *c;
    struct wgs_op *op;
    struct wgs_blob *b;
    struct old_file *of, *of_next;
    struct list blobs, old_files;
    WCHAR *dir = NULL, *path;
    UINT64 used, new_size = 0;
    BYTE old_number;
    BOOL is_new, linked = FALSE;
    HRESULT hr;

    list_init( &blobs );
    list_init( &old_files );
    if ((hr = provider_reload( p )) != S_OK) return hr;
    if (p->read_only) return E_GS_NO_ACCESS;

    if (!(c = provider_find( p, container->name )))
    {
        if (!(c = calloc( 1, sizeof(*c) ))) return E_OUTOFMEMORY;
        c->name = wide_dup( container->name );
        c->display_name = wide_dup( update->display_name ? update->display_name : container->name );
        c->flags = 1;
        CoCreateGuid( &c->folder );
        is_new = TRUE;
    }
    else is_new = FALSE;
    old_number = c->number;

    if (!(dir = container_dir( p, c )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (!is_new && FAILED( hr = manifest_read( dir, c, &blobs ) )) goto done;

    /* work out the resulting blob table before touching the disk */
    LIST_FOR_EACH_ENTRY( op, &update->ops, struct wgs_op, entry )
    {
        struct wgs_blob *found = blobs_find( &blobs, op->name );

        if (found)
        {
            if (!(of = malloc( sizeof(*of) )))
            {
                hr = E_OUTOFMEMORY;
                goto done;
            }
            of->file = found->file;
            list_add_tail( &old_files, &of->entry );
        }
        if (op->is_delete)
        {
            if (found)
            {
                list_remove( &found->entry );
                free( found );
            }
            continue;
        }
        if (!found)
        {
            if (!(found = calloc( 1, sizeof(*found) )))
            {
                hr = E_OUTOFMEMORY;
                goto done;
            }
            wcscpy( found->name, op->name );
            list_add_tail( &blobs, &found->entry );
        }
        CoCreateGuid( &found->file );
        found->file_alt = found->file;
        found->size = op->size;
        op->file = found->file;
    }
    LIST_FOR_EACH_ENTRY( b, &blobs, struct wgs_blob, entry ) new_size += b->size;

    used = provider_used( p ) - (is_new ? 0 : c->size);
    if (used + new_size > p->index.quota)
    {
        WARN( "quota exceeded: %I64u used, %I64u for %s, quota %I64u.\n", used, new_size,
              debugstr_w( container->name ), p->index.quota );
        hr = E_GS_QUOTA_EXCEEDED;
        goto done;
    }

    /* new blob files first, then the manifest that references them, then the
     * index: a failure part way leaves the previous generation intact */
    if (!make_directory( dir ))
    {
        hr = E_GS_OUT_OF_LOCAL_STORAGE;
        goto done;
    }
    LIST_FOR_EACH_ENTRY( op, &update->ops, struct wgs_op, entry )
    {
        BOOL ok;

        if (op->is_delete) continue;
        if (!(path = blob_path( dir, &op->file )))
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        ok = write_whole_file( path, op->data, op->size );
        if (!ok) ERR( "cannot write %s, error %lu.\n", debugstr_w( path ), GetLastError() );
        free( path );
        if (!ok)
        {
            hr = E_GS_OUT_OF_LOCAL_STORAGE;
            goto done;
        }
    }

    c->number = old_number + 1;
    if (!manifest_write( dir, c->number, &blobs ))
    {
        c->number = old_number;
        hr = E_GS_OUT_OF_LOCAL_STORAGE;
        goto done;
    }
    if (update->display_name)
    {
        free( c->display_name );
        c->display_name = wide_dup( update->display_name );
    }
    free( c->etag );
    if ((c->etag = malloc( 32 * sizeof(WCHAR) )))
        swprintf( c->etag, 32, L"\"0x%I64X\"", filetime_now() + TICKS_1601_TO_0001 );
    c->mtime = filetime_now();
    c->size = new_size;
    if (is_new)
    {
        list_add_tail( &p->index.containers, &c->entry );
        linked = TRUE;
    }
    if (FAILED( hr = provider_commit( p ) ))
    {
        if (linked)
        {
            list_remove( &c->entry );
            linked = FALSE;
        }
        goto done;
    }

    /* the update is on disk; what it superseded can go */
    if (!is_new && (path = manifest_path( dir, old_number )))
    {
        DeleteFileW( path );
        free( path );
    }
    LIST_FOR_EACH_ENTRY( of, &old_files, struct old_file, entry )
    {
        if ((path = blob_path( dir, &of->file )))
        {
            DeleteFileW( path );
            free( path );
        }
    }
    TRACE( "container %s now generation %u, %I64u bytes.\n", debugstr_w( c->name ), c->number, c->size );
    hr = S_OK;

done:
    if (is_new && !linked) free_container( c );
    LIST_FOR_EACH_ENTRY_SAFE( of, of_next, &old_files, struct old_file, entry )
    {
        list_remove( &of->entry );
        free( of );
    }
    blobs_free( &blobs );
    free( dir );
    return hr;
}

static HRESULT delete_container( struct XGameSaveProvider *p, const char *name )
{
    struct wgs_container *c;
    WCHAR *name_w, *dir, *pattern;
    WIN32_FIND_DATAW data;
    HANDLE find;
    HRESULT hr;

    if (!(name_w = utf8_to_wide( name ))) return E_OUTOFMEMORY;
    if ((hr = provider_reload( p )) != S_OK || !(c = provider_find( p, name_w )))
    {
        free( name_w );
        return hr;
    }
    free( name_w );
    if (p->read_only) return E_GS_NO_ACCESS;

    if ((dir = container_dir( p, c )))
    {
        if ((pattern = path_join( dir, L"*" )))
        {
            if ((find = FindFirstFileW( pattern, &data )) != INVALID_HANDLE_VALUE)
            {
                do
                {
                    WCHAR *path;

                    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    if (!(path = path_join( dir, data.cFileName ))) continue;
                    DeleteFileW( path );
                    free( path );
                } while (FindNextFileW( find, &data ));
                FindClose( find );
            }
            free( pattern );
        }
        RemoveDirectoryW( dir );
        free( dir );
    }
    list_remove( &c->entry );
    free_container( c );
    return provider_commit( p );
}

static struct wgs_op *update_find_op( struct XGameSaveUpdate *update, const WCHAR *name )
{
    struct wgs_op *op;

    LIST_FOR_EACH_ENTRY( op, &update->ops, struct wgs_op, entry )
        if (!_wcsicmp( op->name, name )) return op;
    return NULL;
}

static void free_op( struct wgs_op *op )
{
    free( op->data );
    free( op );
}

/* Queue a write or delete. A later operation on the same blob replaces an
 * earlier one, so the queue never holds two entries for one name. */
static HRESULT queue_op( struct XGameSaveUpdate *update, const char *blob_name, BOOL is_delete,
                         const BYTE *data, SIZE_T size )
{
    WCHAR *name;
    struct wgs_op *op;

    if (!blob_name || !*blob_name) return E_INVALIDARG;
    if (!(name = utf8_to_wide( blob_name ))) return E_OUTOFMEMORY;
    if (wcslen( name ) >= WGS_BLOB_NAME_CHARS)
    {
        WARN( "blob name %s too long.\n", debugstr_a( blob_name ) );
        free( name );
        return E_INVALIDARG;
    }
    if ((op = update_find_op( update, name )))
    {
        list_remove( &op->entry );
        update->total_size -= op->size;
        free_op( op );
    }
    if (!is_delete && update->total_size + size > GS_MAX_UPDATE_SIZE)
    {
        WARN( "update would grow to %Iu bytes.\n", update->total_size + size );
        free( name );
        return E_GS_UPDATE_TOO_BIG;
    }
    if (!(op = calloc( 1, sizeof(*op) )))
    {
        free( name );
        return E_OUTOFMEMORY;
    }
    wcscpy( op->name, name );
    free( name );
    op->is_delete = is_delete;
    if (!is_delete && size)
    {
        if (!(op->data = malloc( size )))
        {
            free( op );
            return E_OUTOFMEMORY;
        }
        memcpy( op->data, data, size );
    }
    op->size = is_delete ? 0 : size;
    list_add_tail( &update->ops, &op->entry );
    update->total_size += op->size;
    return S_OK;
}

/* ---- interface ---------------------------------------------------------- */

struct x_game_save
{
    IXGameSaveImpl3 IXGameSaveImpl3_iface;
    LONG ref;
};

static inline struct x_game_save *impl_from_IXGameSaveImpl3( IXGameSaveImpl3 *iface )
{
    return CONTAINING_RECORD( iface, struct x_game_save, IXGameSaveImpl3_iface );
}

static HRESULT WINAPI x_game_save_QueryInterface( IXGameSaveImpl3 *iface, REFIID iid, void **out )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl3( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown        ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl  ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl2 ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl3 ))
    {
        IXGameSaveImpl_AddRef( *out = &impl->IXGameSaveImpl3_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_game_save_AddRef( IXGameSaveImpl3 *iface )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl3( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_game_save_Release( IXGameSaveImpl3 *iface )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl3( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProvider( IXGameSaveImpl3 *iface, XUserHandle requestingUser, const char *configurationId, BOOLEAN syncOnDemand, XGameSaveProviderHandle *provider )
{
    TRACE( "iface %p, requestingUser %p, configurationId %s, syncOnDemand %d, provider %p.\n", iface, requestingUser, debugstr_a( configurationId ), syncOnDemand, provider );

    if (!provider) return E_INVALIDARG;
    return create_provider( requestingUser, configurationId, provider );
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProviderAsync( IXGameSaveImpl3 *iface, XUserHandle requestingUser, const char *configurationId, BOOLEAN syncOnDemand, XAsyncBlock *async )
{
    struct XGameSaveProvider *provider = NULL;
    HRESULT hr;

    TRACE( "iface %p, requestingUser %p, configurationId %s, syncOnDemand %d, async %p.\n", iface, requestingUser, debugstr_a( configurationId ), syncOnDemand, async );

    if (!async) return E_INVALIDARG;
    hr = create_provider( requestingUser, configurationId, &provider );
    xgr_async_complete( async, hr, provider );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProviderResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, XGameSaveProviderHandle *provider )
{
    HRESULT hr;
    void *payload;

    TRACE( "iface %p, async %p, provider %p.\n", iface, async, provider );

    if (!provider) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, &payload )) return E_PENDING;
    *provider = SUCCEEDED( hr ) ? payload : NULL;
    return hr;
}

static void WINAPI x_game_save_XGameSaveCloseProvider( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider )
{
    TRACE( "iface %p, provider %p.\n", iface, provider );

    if (provider) provider_release( provider );
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuota( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, INT64 *remainingQuota )
{
    HRESULT hr;

    TRACE( "iface %p, provider %p, remainingQuota %p.\n", iface, provider, remainingQuota );

    if (!provider || !remainingQuota) return E_INVALIDARG;
    EnterCriticalSection( &gs_cs );
    if ((hr = provider_reload( provider )) == S_OK) *remainingQuota = provider_remaining( provider );
    LeaveCriticalSection( &gs_cs );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuotaAsync( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, XAsyncBlock *async )
{
    INT64 *remaining;
    HRESULT hr;

    TRACE( "iface %p, provider %p, async %p.\n", iface, provider, async );

    if (!async) return E_INVALIDARG;
    if (!(remaining = malloc( sizeof(*remaining) ))) return E_OUTOFMEMORY;
    if (FAILED( hr = x_game_save_XGameSaveGetRemainingQuota( iface, provider, remaining ) ))
    {
        free( remaining );
        remaining = NULL;
    }
    xgr_async_complete( async, hr, remaining );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuotaResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, INT64 *remainingQuota )
{
    INT64 *remaining;
    HRESULT hr;

    TRACE( "iface %p, async %p, remainingQuota %p.\n", iface, async, remainingQuota );

    if (!remainingQuota) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, (void **)&remaining )) return E_PENDING;
    if (FAILED( hr )) return hr;
    if (!remaining) return E_INVALIDARG;
    *remainingQuota = *remaining;
    free( remaining );
    async->internal[2] = NULL;
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainer( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName )
{
    HRESULT hr;

    TRACE( "iface %p, provider %p, containerName %s.\n", iface, provider, debugstr_a( containerName ) );

    if (!provider) return E_INVALIDARG;
    if (!valid_container_name( containerName )) return E_GS_INVALID_CONTAINER_NAME;
    EnterCriticalSection( &gs_cs );
    hr = delete_container( provider, containerName );
    LeaveCriticalSection( &gs_cs );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainerAsync( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName, XAsyncBlock *async )
{
    HRESULT hr;

    TRACE( "iface %p, provider %p, containerName %s, async %p.\n", iface, provider, debugstr_a( containerName ), async );

    if (!async) return E_INVALIDARG;
    hr = x_game_save_XGameSaveDeleteContainer( iface, provider, containerName );
    xgr_async_complete( async, hr, NULL );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainerResult( IXGameSaveImpl3 *iface, XAsyncBlock *async )
{
    HRESULT hr;

    TRACE( "iface %p, async %p.\n", iface, async );

    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveGetContainerInfo( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName, void *context, XGameSaveContainerInfoCallback *callback )
{
    struct wgs_container *c;
    WCHAR *name;
    HRESULT hr;

    TRACE( "iface %p, provider %p, containerName %s, context %p, callback %p.\n", iface, provider, debugstr_a( containerName ), context, callback );

    if (!provider || !callback) return E_INVALIDARG;
    if (!valid_container_name( containerName )) return E_GS_INVALID_CONTAINER_NAME;
    if (!(name = utf8_to_wide( containerName ))) return E_OUTOFMEMORY;
    EnterCriticalSection( &gs_cs );
    if ((hr = provider_reload( provider )) == S_OK && (c = provider_find( provider, name )))
        report_container( provider, c, context, callback );
    LeaveCriticalSection( &gs_cs );
    free( name );
    return hr;
}

static HRESULT enumerate_containers( struct XGameSaveProvider *provider, const char *prefix, void *context, XGameSaveContainerInfoCallback *callback )
{
    struct wgs_container *c;
    WCHAR *prefix_w = NULL, **names;
    SIZE_T prefix_len = 0;
    UINT32 count = 0, i;
    HRESULT hr;

    if (!provider || !callback) return E_INVALIDARG;
    if (prefix && *prefix)
    {
        if (!(prefix_w = utf8_to_wide( prefix ))) return E_OUTOFMEMORY;
        prefix_len = wcslen( prefix_w );
    }
    EnterCriticalSection( &gs_cs );
    if ((hr = provider_reload( provider )) == S_OK)
    {
        /* the callback may call back in and reload the list, so walk a
         * snapshot of the names and look each one up again */
        LIST_FOR_EACH_ENTRY( c, &provider->index.containers, struct wgs_container, entry ) count++;
        if (!(names = calloc( count + 1, sizeof(*names) ))) hr = E_OUTOFMEMORY;
        else
        {
            i = 0;
            LIST_FOR_EACH_ENTRY( c, &provider->index.containers, struct wgs_container, entry )
                names[i++] = wide_dup( c->name );
            for (i = 0; i < count; i++)
            {
                if (!names[i] || (prefix_w && _wcsnicmp( names[i], prefix_w, prefix_len ))) continue;
                if (!(c = provider_find( provider, names[i] ))) continue;
                if (!report_container( provider, c, context, callback )) break;
            }
            for (i = 0; i < count; i++) free( names[i] );
            free( names );
        }
    }
    LeaveCriticalSection( &gs_cs );
    free( prefix_w );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateContainerInfo( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, void *context, XGameSaveContainerInfoCallback *callback )
{
    TRACE( "iface %p, provider %p, context %p, callback %p.\n", iface, provider, context, callback );

    return enumerate_containers( provider, NULL, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateContainerInfoByName( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerNamePrefix, void *context, XGameSaveContainerInfoCallback *callback )
{
    TRACE( "iface %p, provider %p, containerNamePrefix %s, context %p, callback %p.\n", iface, provider, debugstr_a( containerNamePrefix ), context, callback );

    return enumerate_containers( provider, containerNamePrefix, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveCreateContainer( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName, XGameSaveContainerHandle *containerContext )
{
    struct XGameSaveContainer *container;

    TRACE( "iface %p, provider %p, containerName %s, containerContext %p.\n", iface, provider, debugstr_a( containerName ), containerContext );

    if (!provider || !containerContext) return E_INVALIDARG;
    if (!valid_container_name( containerName )) return E_GS_INVALID_CONTAINER_NAME;
    if (!(container = calloc( 1, sizeof(*container) ))) return E_OUTOFMEMORY;
    if (!(container->name = utf8_to_wide( containerName )))
    {
        free( container );
        return E_OUTOFMEMORY;
    }
    list_init( &container->strings );
    container->provider = provider;
    provider_addref( provider );
    /* nothing reaches the disk until an update is submitted */
    *containerContext = container;
    return S_OK;
}

static void WINAPI x_game_save_XGameSaveCloseContainer( IXGameSaveImpl3 *iface, XGameSaveContainerHandle context )
{
    struct x_string_entry *e, *next;

    TRACE( "iface %p, context %p.\n", iface, context );

    if (!context) return;
    EnterCriticalSection( &gs_cs );
    LIST_FOR_EACH_ENTRY_SAFE( e, next, &context->strings, struct x_string_entry, entry )
    {
        list_remove( &e->entry );
        free( e );
    }
    LeaveCriticalSection( &gs_cs );
    provider_release( context->provider );
    free( context->name );
    free( context );
}

static HRESULT enumerate_blobs( struct XGameSaveContainer *container, const char *prefix, void *context, XGameSaveBlobInfoCallback *callback )
{
    struct XGameSaveProvider *p;
    struct wgs_container *c;
    struct wgs_blob *b;
    struct list blobs;
    WCHAR *prefix_w = NULL, *dir;
    SIZE_T prefix_len = 0;
    HRESULT hr;

    if (!container || !callback) return E_INVALIDARG;
    p = container->provider;
    if (prefix && *prefix)
    {
        if (!(prefix_w = utf8_to_wide( prefix ))) return E_OUTOFMEMORY;
        prefix_len = wcslen( prefix_w );
    }
    EnterCriticalSection( &gs_cs );
    if ((hr = provider_reload( p )) == S_OK && (c = provider_find( p, container->name )))
    {
        if (!(dir = container_dir( p, c ))) hr = E_OUTOFMEMORY;
        else
        {
            if (SUCCEEDED( hr = manifest_read( dir, c, &blobs ) ))
            {
                LIST_FOR_EACH_ENTRY( b, &blobs, struct wgs_blob, entry )
                {
                    XGameSaveBlobInfo info;
                    BOOL cont;

                    if (prefix_w && _wcsnicmp( b->name, prefix_w, prefix_len )) continue;
                    if (!(info.name = wide_to_utf8( b->name ))) continue;
                    info.size = b->size;
                    TRACE( "blob %s, %u bytes.\n", debugstr_a( info.name ), info.size );
                    cont = callback( &info, context );
                    free( (char *)info.name );
                    if (!cont) break;
                }
                blobs_free( &blobs );
                hr = S_OK;
            }
            free( dir );
        }
    }
    LeaveCriticalSection( &gs_cs );
    free( prefix_w );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateBlobInfo( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, void *context, XGameSaveBlobInfoCallback *callback )
{
    TRACE( "iface %p, container %p, context %p, callback %p.\n", iface, container, context, callback );

    return enumerate_blobs( container, NULL, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateBlobInfoByName( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char *blobNamePrefix, void *context, XGameSaveBlobInfoCallback *callback )
{
    TRACE( "iface %p, container %p, blobNamePrefix %s, context %p, callback %p.\n", iface, container, debugstr_a( blobNamePrefix ), context, callback );

    return enumerate_blobs( container, blobNamePrefix, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobData( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char **blobNames, UINT32 *countOfBlobs, SIZE_T blobsSize, XGameSaveBlob *blobData )
{
    HRESULT hr;

    TRACE( "iface %p, container %p, blobNames %p, countOfBlobs %p, blobsSize %Iu, blobData %p.\n", iface, container, blobNames, countOfBlobs, blobsSize, blobData );

    if (!container || !countOfBlobs) return E_INVALIDARG;
    EnterCriticalSection( &gs_cs );
    hr = read_blobs( container, blobNames, countOfBlobs, blobsSize, blobData, NULL );
    LeaveCriticalSection( &gs_cs );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobDataAsync( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char **blobNames, UINT32 countOfBlobs, XAsyncBlock *async )
{
    struct read_request *req;
    SIZE_T required = 0;
    UINT32 n = countOfBlobs, i;
    HRESULT hr;

    TRACE( "iface %p, container %p, blobNames %p, countOfBlobs %u, async %p.\n", iface, container, blobNames, countOfBlobs, async );

    if (!container || !async) return E_INVALIDARG;
    if (!(req = calloc( 1, sizeof(*req) ))) return E_OUTOFMEMORY;
    req->container = container;
    if (blobNames)
    {
        req->count = n;
        if (!(req->names = calloc( n + 1, sizeof(*req->names) )))
        {
            free( req );
            return E_OUTOFMEMORY;
        }
        for (i = 0; i < n; i++)
        {
            if (!(req->names[i] = narrow_dup( blobNames[i] )))
            {
                free_read_request( req );
                return E_OUTOFMEMORY;
            }
        }
    }

    /* validate now and report the buffer size; the copy happens in *Result */
    EnterCriticalSection( &gs_cs );
    hr = read_blobs( container, (const char **)req->names, &n, 0, NULL, &required );
    LeaveCriticalSection( &gs_cs );
    if (FAILED( hr ))
    {
        free_read_request( req );
        req = NULL;
    }
    xgr_async_complete_ex( async, hr, req, required );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobDataResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, SIZE_T blobsSize, XGameSaveBlob *blobData, UINT32 *countOfBlobs )
{
    struct read_request *req;
    UINT32 n;
    HRESULT hr;

    TRACE( "iface %p, async %p, blobsSize %Iu, blobData %p, countOfBlobs %p.\n", iface, async, blobsSize, blobData, countOfBlobs );

    if (!xgr_async_completed( async, &hr, (void **)&req )) return E_PENDING;
    if (FAILED( hr )) return hr;
    if (!req) return E_INVALIDARG;

    n = req->count;
    EnterCriticalSection( &gs_cs );
    hr = read_blobs( req->container, (const char **)req->names, &n, blobsSize, blobData, NULL );
    LeaveCriticalSection( &gs_cs );
    if (countOfBlobs) *countOfBlobs = n;
    if (hr != E_GS_PROVIDED_BUFFER_TOO_SMALL)
    {
        /* keep the request around for a retry with a bigger buffer only */
        async->internal[2] = NULL;
        free_read_request( req );
    }
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveCreateUpdate( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char *containerDisplayName, XGameSaveUpdateHandle *updateContext )
{
    struct XGameSaveUpdate *update;

    TRACE( "iface %p, container %p, containerDisplayName %s, updateContext %p.\n", iface, container, debugstr_a( containerDisplayName ), updateContext );

    if (!container || !updateContext) return E_INVALIDARG;
    if (!(update = calloc( 1, sizeof(*update) ))) return E_OUTOFMEMORY;
    if (containerDisplayName && !(update->display_name = utf8_to_wide( containerDisplayName )))
    {
        free( update );
        return E_OUTOFMEMORY;
    }
    list_init( &update->ops );
    update->container = container;
    *updateContext = update;
    return S_OK;
}

static void WINAPI x_game_save_XGameSaveCloseUpdate( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle context )
{
    struct wgs_op *op, *next;

    TRACE( "iface %p, context %p.\n", iface, context );

    if (!context) return;
    LIST_FOR_EACH_ENTRY_SAFE( op, next, &context->ops, struct wgs_op, entry )
    {
        list_remove( &op->entry );
        free_op( op );
    }
    free( context->display_name );
    free( context );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitBlobWrite( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext, const char *blobName, UINT8 *data, SIZE_T byteCount )
{
    TRACE( "iface %p, updateContext %p, blobName %s, data %p, byteCount %Iu.\n", iface, updateContext, debugstr_a( blobName ), data, byteCount );

    if (!updateContext || (!data && byteCount)) return E_INVALIDARG;
    return queue_op( updateContext, blobName, FALSE, data, byteCount );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitBlobDelete( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext, const char *blobName )
{
    TRACE( "iface %p, updateContext %p, blobName %s.\n", iface, updateContext, debugstr_a( blobName ) );

    if (!updateContext) return E_INVALIDARG;
    return queue_op( updateContext, blobName, TRUE, NULL, 0 );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdate( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext )
{
    HRESULT hr;

    TRACE( "iface %p, updateContext %p.\n", iface, updateContext );

    if (!updateContext) return E_INVALIDARG;
    EnterCriticalSection( &gs_cs );
    hr = submit_update( updateContext );
    LeaveCriticalSection( &gs_cs );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdateAsync( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext, XAsyncBlock *async )
{
    HRESULT hr;

    TRACE( "iface %p, updateContext %p, async %p.\n", iface, updateContext, async );

    if (!async) return E_INVALIDARG;
    hr = x_game_save_XGameSaveSubmitUpdate( iface, updateContext );
    xgr_async_complete( async, hr, NULL );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdateResult( IXGameSaveImpl3 *iface, XAsyncBlock *async )
{
    HRESULT hr;

    TRACE( "iface %p, async %p.\n", iface, async );

    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    return hr;
}

/* XGameSaveFiles: a plain folder the title reads and writes itself. It sits
 * inside the same per-user directory so everything for one title stays put. */
static HRESULT WINAPI x_game_save_XGameSaveFilesGetFolderWithUiAsync( IXGameSaveImpl3 *iface, XUserHandle requestingUser, const char *configurationId, XAsyncBlock *async )
{
    struct XGameSaveProvider *provider;
    WCHAR *dir;
    char *folder = NULL;
    HRESULT hr;

    TRACE( "iface %p, requestingUser %p, configurationId %s, async %p.\n", iface, requestingUser, debugstr_a( configurationId ), async );

    if (!async) return E_INVALIDARG;
    if (SUCCEEDED( hr = create_provider( requestingUser, configurationId, &provider ) ))
    {
        if ((dir = path_join( provider->dir, L"files" )) && make_directory( dir )) folder = wide_to_utf8( dir );
        if (!folder) hr = E_GS_OUT_OF_LOCAL_STORAGE;
        free( dir );
        provider_release( provider );
    }
    TRACE( "folder %s, hr %#lx.\n", debugstr_a( folder ), hr );
    xgr_async_complete_ex( async, hr, folder, folder ? strlen( folder ) + 1 : 0 );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveFilesGetFolderWithUiResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, SIZE_T folderSize, char *folderResult )
{
    char *folder;
    SIZE_T len;
    HRESULT hr;

    TRACE( "iface %p, async %p, folderSize %Iu, folderResult %p.\n", iface, async, folderSize, folderResult );

    if (!xgr_async_completed( async, &hr, (void **)&folder )) return E_PENDING;
    if (FAILED( hr )) return hr;
    if (!folder) return E_INVALIDARG;
    len = strlen( folder ) + 1;
    if (!folderResult || folderSize < len) return E_GS_PROVIDED_BUFFER_TOO_SMALL;
    memcpy( folderResult, folder, len );
    free( folder );
    async->internal[2] = NULL;
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveFilesGetRemainingQuota( IXGameSaveImpl3 *iface, XUserHandle userContext, const char *configurationId, INT64 *remainingQuota )
{
    struct XGameSaveProvider *provider;
    HRESULT hr;

    TRACE( "iface %p, userContext %p, configurationId %s, remainingQuota %p.\n", iface, userContext, debugstr_a( configurationId ), remainingQuota );

    if (!remainingQuota) return E_INVALIDARG;
    if (FAILED( hr = create_provider( userContext, configurationId, &provider ) )) return hr;
    EnterCriticalSection( &gs_cs );
    *remainingQuota = provider_remaining( provider );
    LeaveCriticalSection( &gs_cs );
    provider_release( provider );
    return S_OK;
}

static const struct IXGameSaveImpl3Vtbl x_game_save_vtbl =
{
    x_game_save_QueryInterface,
    x_game_save_AddRef,
    x_game_save_Release,
    /* IXGameSaveImpl methods */
    x_game_save_XGameSaveInitializeProvider,
    x_game_save_XGameSaveInitializeProviderAsync,
    x_game_save_XGameSaveInitializeProviderResult,
    x_game_save_XGameSaveCloseProvider,
    x_game_save_XGameSaveGetRemainingQuota,
    x_game_save_XGameSaveGetRemainingQuotaAsync,
    x_game_save_XGameSaveGetRemainingQuotaResult,
    x_game_save_XGameSaveDeleteContainer,
    x_game_save_XGameSaveDeleteContainerAsync,
    x_game_save_XGameSaveDeleteContainerResult,
    x_game_save_XGameSaveGetContainerInfo,
    x_game_save_XGameSaveEnumerateContainerInfo,
    x_game_save_XGameSaveEnumerateContainerInfoByName,
    x_game_save_XGameSaveCreateContainer,
    x_game_save_XGameSaveCloseContainer,
    x_game_save_XGameSaveEnumerateBlobInfo,
    x_game_save_XGameSaveEnumerateBlobInfoByName,
    x_game_save_XGameSaveReadBlobData,
    x_game_save_XGameSaveReadBlobDataAsync,
    x_game_save_XGameSaveReadBlobDataResult,
    x_game_save_XGameSaveCreateUpdate,
    x_game_save_XGameSaveCloseUpdate,
    x_game_save_XGameSaveSubmitBlobWrite,
    x_game_save_XGameSaveSubmitBlobDelete,
    x_game_save_XGameSaveSubmitUpdate,
    x_game_save_XGameSaveSubmitUpdateAsync,
    x_game_save_XGameSaveSubmitUpdateResult,
    /* IXGameSaveImpl2 methods */
    x_game_save_XGameSaveFilesGetFolderWithUiAsync,
    x_game_save_XGameSaveFilesGetFolderWithUiResult,
    x_game_save_XGameSaveFilesGetRemainingQuota,
};

static struct x_game_save x_game_save =
{
    {&x_game_save_vtbl},
    0,
};

IXGameSaveImpl *x_game_save_impl = (IXGameSaveImpl *)&x_game_save.IXGameSaveImpl3_iface;
