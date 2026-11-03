/*
 * Unix-side implementation for xgameruntime: talking to xodus-service
 *
 * Copyright 2026 Xodus GDK-on-Linux project
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"

#include "wine/debug.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

/* xodus-service's framing: a magic, then a 16-bit type and length, then the
 * body. The reply comes back with the request's type plus one. */
#define XODUS_XML_MAGIC          0x58445358
#define XODUS_XSTS_TOKEN_REQUEST 5

/* Escape text for XML. A URL's query string routinely contains '&', which
 * starts an entity reference -- embedding one raw makes the document
 * ill-formed and the service rejects the whole request, which is what stopped
 * profile lookups (their URLs carry query parameters) while plain ones worked. */
static char *xml_escape( const char *in )
{
    static const struct { char ch; const char *rep; } map[] = {
        { '&', "&amp;" }, { '<', "&lt;" }, { '>', "&gt;" },
        { '"', "&quot;" }, { '\'', "&apos;" },
    };
    size_t len = 0, i;
    const char *p;
    char *out, *o;

    if (!in) in = "";
    for (p = in; *p; p++)
    {
        size_t add = 1;
        for (i = 0; i < ARRAY_SIZE(map); i++)
            if (*p == map[i].ch) { add = strlen( map[i].rep ); break; }
        len += add;
    }
    if (!(out = malloc( len + 1 ))) return NULL;
    for (p = in, o = out; *p; p++)
    {
        const char *rep = NULL;
        for (i = 0; i < ARRAY_SIZE(map); i++)
            if (*p == map[i].ch) { rep = map[i].rep; break; }
        if (rep) { strcpy( o, rep ); o += strlen( rep ); }
        else *o++ = *p;
    }
    *o = 0;
    return out;
}

/* Standard base64, for carrying a request body through the XML. */
static char *base64_encode( const void *data, size_t len )
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *p = data;
    size_t i, o = 0;
    char *out;

    if (!(out = malloc( ((len + 2) / 3) * 4 + 1 ))) return NULL;
    for (i = 0; i + 2 < len; i += 3)
    {
        out[o++] = tbl[p[i] >> 2];
        out[o++] = tbl[((p[i] & 3) << 4) | (p[i+1] >> 4)];
        out[o++] = tbl[((p[i+1] & 15) << 2) | (p[i+2] >> 6)];
        out[o++] = tbl[p[i+2] & 63];
    }
    if (i < len)
    {
        out[o++] = tbl[p[i] >> 2];
        if (i + 1 < len)
        {
            out[o++] = tbl[((p[i] & 3) << 4) | (p[i+1] >> 4)];
            out[o++] = tbl[(p[i+1] & 15) << 2];
        }
        else
        {
            out[o++] = tbl[(p[i] & 3) << 4];
            out[o++] = '=';
        }
        out[o++] = '=';
    }
    out[o] = 0;
    return out;
}

static int connect_service( void )
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    const char *runtime_dir = getenv( "XDG_RUNTIME_DIR" );
    int fd;

    if (!runtime_dir || !runtime_dir[0])
    {
        WARN( "XDG_RUNTIME_DIR is not set; cannot reach xodus-service.\n" );
        return -1;
    }
    if (snprintf( addr.sun_path, sizeof(addr.sun_path), "%s/xodus.sock", runtime_dir ) >= (int)sizeof(addr.sun_path))
    {
        WARN( "socket path does not fit.\n" );
        return -1;
    }

    if ((fd = socket( AF_UNIX, SOCK_STREAM, 0 )) < 0) return -1;
    if (connect( fd, (struct sockaddr *)&addr, sizeof(addr) ))
    {
        WARN( "xodus-service is not listening on %s (%s); no Xbox Live sign-in.\n",
              addr.sun_path, strerror( errno ) );
        close( fd );
        return -1;
    }
    return fd;
}

static BOOL write_all( int fd, const void *buf, size_t len )
{
    const char *p = buf;

    while (len)
    {
        ssize_t n = write( fd, p, len );
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return FALSE;
        }
        if (!n) return FALSE;
        p += n;
        len -= n;
    }
    return TRUE;
}

static BOOL read_all( int fd, void *buf, size_t len )
{
    char *p = buf;

    while (len)
    {
        ssize_t n = read( fd, p, len );
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return FALSE;
        }
        if (!n) return FALSE;
        p += n;
        len -= n;
    }
    return TRUE;
}

/* Copy the text between <tag> and </tag> into out. The service's replies are
 * small, flat and machine-generated, so a scan is enough and saves dragging an
 * XML parser into the unix side. */
static void extract_tag( const char *xml, const char *tag, char *out, size_t out_len )
{
    char open_tag[64], close_tag[64];
    const char *start, *end;
    size_t len;

    out[0] = 0;
    snprintf( open_tag, sizeof(open_tag), "<%s>", tag );
    snprintf( close_tag, sizeof(close_tag), "</%s>", tag );

    if (!(start = strstr( xml, open_tag ))) return;
    start += strlen( open_tag );
    if (!(end = strstr( start, close_tag ))) return;

    len = end - start;
    if (len >= out_len) len = out_len - 1;
    memcpy( out, start, len );
    out[len] = 0;
}

static NTSTATUS xsts_token_once( void *args )
{
    struct xsts_token_params *params = args;
    char *request = NULL, header[8], *body = NULL, *body_b64 = NULL;
    char *url_esc = NULL, *method_esc = NULL;
    unsigned int magic, size;
    unsigned short type, len;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    char expiry[XGR_IDENTITY_MAX];
    int fd, body_len;
    size_t request_size;

    params->token[0] = params->xuid[0] = params->gamertag[0] = params->signature[0] = 0;
    params->expiry = 0;

    if ((fd = connect_service()) < 0) return STATUS_NOT_FOUND;

    /* The signature covers the body, so it has to travel with the ask; base64
     * keeps arbitrary bytes safe inside the XML. */
    if (params->body && params->body_size)
    {
        if (!(body_b64 = base64_encode( params->body, params->body_size ))) goto done;
    }

    if (!(url_esc = xml_escape( params->url )) ||
        !(method_esc = xml_escape( params->method ? params->method : "GET" )))
    { status = STATUS_NO_MEMORY; goto done; }

    request_size = 512 + strlen( url_esc ) + strlen( method_esc ) +
                   (body_b64 ? strlen( body_b64 ) : 0);
    if (!(request = malloc( request_size ))) { status = STATUS_NO_MEMORY; goto done; }

    body_len = snprintf( request, request_size,
                         "<XSTSTokenRequest><Url>%s</Url><RelyingParty></RelyingParty>"
                         "<Method>%s</Method><BodyBase64>%s</BodyBase64></XSTSTokenRequest>",
                         url_esc, method_esc, body_b64 ? body_b64 : "" );
    if (body_len < 0 || body_len >= (int)request_size) goto done;
    if (body_len > 0xffff)
    {
        WARN( "request body is too large to frame.\n" );
        goto done;
    }

    magic = XODUS_XML_MAGIC;
    type = XODUS_XSTS_TOKEN_REQUEST;
    len = body_len;
    memcpy( header, &magic, 4 );
    memcpy( header + 4, &type, 2 );
    memcpy( header + 6, &len, 2 );

    if (!write_all( fd, header, sizeof(header) )) goto done;
    if (!write_all( fd, request, body_len )) goto done;

    if (!read_all( fd, header, sizeof(header) )) goto done;
    memcpy( &magic, header, 4 );
    memcpy( &type, header + 4, 2 );
    memcpy( &len, header + 6, 2 );
    if (magic != XODUS_XML_MAGIC)
    {
        WARN( "unexpected reply magic %#x.\n", magic );
        goto done;
    }
    if (!len)
    {
        WARN( "xodus-service returned no token; the account may not be signed in.\n" );
        status = STATUS_LOGON_FAILURE;
        goto done;
    }

    size = len;
    if (!(body = malloc( size + 1 ))) { status = STATUS_NO_MEMORY; goto done; }
    if (!read_all( fd, body, size )) goto done;
    body[size] = 0;

    extract_tag( body, "Token", params->token, sizeof(params->token) );
    extract_tag( body, "Xuid", params->xuid, sizeof(params->xuid) );
    extract_tag( body, "Gamertag", params->gamertag, sizeof(params->gamertag) );
    extract_tag( body, "Signature", params->signature, sizeof(params->signature) );
    extract_tag( body, "Expiry", expiry, sizeof(expiry) );
    params->expiry = atoll( expiry );

    if (!params->token[0])
    {
        status = STATUS_LOGON_FAILURE;
        goto done;
    }

    TRACE( "got a %zu-character token for %s (gamertag %s).\n",
           strlen( params->token ), debugstr_a( params->url ), debugstr_a( params->gamertag ) );
    status = STATUS_SUCCESS;

done:
    free( url_esc );
    free( method_esc );
    free( request );
    free( body_b64 );
    free( body );
    close( fd );
    return status;
}

/* Ask again after a transient failure. The service is a separate process, so a
 * request can lose a race with it at startup -- and the caller here is usually
 * resolving the signed-in account, where giving up once meant the title showed
 * nobody signed in until it was restarted. A refused sign-in is not transient
 * and is returned as it stands. */
static NTSTATUS xsts_token( void *args )
{
    static const int backoff_us[] = { 50000, 150000, 300000 };
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(backoff_us) + 1; i++)
    {
        status = xsts_token_once( args );
        if (status == STATUS_SUCCESS || status == STATUS_LOGON_FAILURE) return status;
        if (i < ARRAY_SIZE(backoff_us))
        {
            TRACE( "token request failed (%#x); retrying.\n", (unsigned int)status );
            usleep( backoff_us[i] );
        }
    }
    return status;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    xsts_token,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );
