/*
 * Unix-side interface for xgameruntime
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

/*
 * A real Xbox Live token can only come from the signed-in Microsoft account,
 * whose credentials live outside the prefix in xodus-service. That service
 * listens on a Unix socket, which a PE module cannot open, so the request goes
 * through this unixlib: the PE side asks for a token for a URL, the Unix side
 * talks to the service and hands back the finished
 * "XBL3.0 x=<user hash>;<token>" header plus the identity that came with it.
 *
 * Buffers are caller-allocated and fixed size to keep the boundary trivial.
 */

#ifndef __WINE_XGAMERUNTIME_UNIXLIB_H
#define __WINE_XGAMERUNTIME_UNIXLIB_H

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/unixlib.h"

#define XGR_TOKEN_MAX     4096
#define XGR_IDENTITY_MAX  64

#define XGR_SIGNATURE_MAX 512

struct xsts_token_params
{
    const char *url;                        /* in:  the URL the title will call */
    const char *method;                     /* in:  its HTTP method */
    const void *body;                       /* in:  its body, may be NULL */
    ULONG body_size;                        /* in */
    const char *title_id;                   /* in:  decimal TitleId, may be NULL */
    const char *title_client_id;            /* in:  MSAAppId, may be NULL */
    char token[XGR_TOKEN_MAX];              /* out: "XBL3.0 x=<uhs>;<token>" */
    char signature[XGR_SIGNATURE_MAX];      /* out: Signature header, may be empty */
    char xuid[XGR_IDENTITY_MAX];            /* out: decimal XUID, may be empty */
    char gamertag[XGR_IDENTITY_MAX];        /* out: may be empty */
    INT64 expiry;                           /* out: unix time the token expires */
};

enum xgr_funcs
{
    unix_xsts_token,
    unix_funcs_count,
};

#define XGR_UNIX_CALL( func, params ) WINE_UNIX_CALL( unix_ ## func, params )

#endif /* __WINE_XGAMERUNTIME_UNIXLIB_H */
