/******************************************************************************
 * $Id$
 *
 * Copyright (c) 2007 Joshua Elsasser
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <assert.h>
#include <errno.h>
#include <event.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bsdqueue.h"
#include "bsdtree.h"
#include "client.h"
#include "bencode.h"
#include "errors.h"
#include "ipc.h"
#include "misc.h"

/* time out server after this many seconds */
#define SERVER_TIMEOUT          ( 15 )

struct con
{
    int                  fd;
    struct ipc_info      ipc;
    struct bufferevent * ev;
};

struct req
{
    enum ipc_msg       id;
    int64_t            tag;
    struct strlist   * strs;
    int64_t            num;
    char             * str;
    size_t             listlen;
    int64_t          * numlist;
    int                types;
    SLIST_ENTRY( req ) next;
};

SLIST_HEAD( reqlist, req );

struct resp
{
    int64_t            tag;
    cl_listfunc        listcb;
    cl_infofunc        infocb;
    cl_hashfunc        hashcb;
    cl_statfunc        statcb;
    RB_ENTRY( resp )   links;
};

RB_HEAD( resptree, resp );

static struct req * addreq   ( enum ipc_msg, int64_t, struct resp ** );
static int     addintlistreq ( enum ipc_msg, size_t, const int * );
static void    didwrite      ( struct bufferevent *, void * );
static void    ohshit        ( struct bufferevent *, short, void * );
static void    canread       ( struct bufferevent *, void * );
static void    flushreqs     ( struct con * );
static int     sendvers      ( struct bufferevent * );
static void    infomsg       ( enum ipc_msg, benc_val_t *, int64_t, void * );
static void    statmsg       ( enum ipc_msg, benc_val_t *, int64_t, void * );
static void    defmsg        ( enum ipc_msg, benc_val_t *, int64_t, void * );
static void    cbdone        ( struct resp * );
static int64_t getinfoint    ( enum ipc_msg, benc_val_t *, int, int64_t );
static char *  getinfostr    ( enum ipc_msg, benc_val_t *, int, char * );
static int     resptagcmp    ( struct resp *, struct resp * );

RB_GENERATE_STATIC( resptree, resp, links, resptagcmp )
INTCMP_FUNC( resptagcmp, resp, tag )

static struct event_base * gl_base   = NULL;
static struct ipc_funcs  * gl_tree   = NULL;
static struct reqlist      gl_reqs   = SLIST_HEAD_INITIALIZER( &gl_reqs );
static struct resptree     gl_resps  = RB_INITIALIZER( &gl_resps );
static int64_t             gl_tag    = 0;

int
client_init( struct event_base * base )
{
    assert( NULL == gl_base && NULL == gl_tree );
    gl_base   = base;
    gl_tree   = ipc_initmsgs();
    if( NULL == gl_tree )
    {
        return -1;
    }

    if( 0 > ipc_addmsg( gl_tree, IPC_MSG_INFO, infomsg ) ||
        0 > ipc_addmsg( gl_tree, IPC_MSG_STAT, statmsg ) )
    {
        return -1;
    }

    ipc_setdefmsg( gl_tree, defmsg );

    return 0;
}

int
client_connect( const char * path )
{
    struct sockaddr_un sun;
    int                fd;
    struct con       * con;

    assert( NULL != gl_base );
    assert( NULL != path );

    bzero( &sun, sizeof sun );
    sun.sun_family = AF_LOCAL;
    strlcpy( sun.sun_path, path, sizeof sun.sun_path );

    fd = socket( AF_UNIX, SOCK_STREAM, 0 );
    if( 0 > fd )
    {
        errnomsg( "failed to create socket" );
        return -1;
    }

    if( 0 > connect( fd, ( struct sockaddr * )&sun, SUN_LEN( &sun ) ) )
    {
        errnomsg( "failed to connect to socket file: %s", path );
        close( fd );
        return -1;
    }
    con = calloc( 1, sizeof *con );
    if( NULL == con )
    {
        mallocmsg( sizeof *con );
        close( fd );
        return -1;
    }
    ipc_newcon( &con->ipc, gl_tree );
    con->fd   = fd;
    con->ev   = bufferevent_new( fd, canread, didwrite, ohshit, con );
    if( NULL == con->ev )
    {
        mallocmsg( -1 );
        close( fd );
        free( con );
        return -1;
    }
    bufferevent_base_set( gl_base, con->ev );
    bufferevent_settimeout( con->ev, SERVER_TIMEOUT, SERVER_TIMEOUT );
    bufferevent_enable( con->ev, EV_READ );
    if( 0 > sendvers( con->ev ) )
    {
        exit( 1 );
    }

    return 0;
}

struct req *
addreq( enum ipc_msg id, int64_t tag, struct resp ** resp )
{
    struct req * req;

    assert( ( 0 < tag && NULL != resp ) || ( 0 >= tag && NULL == resp ) );

    req = calloc( 1, sizeof *req );
    if( NULL == req )
    {
        mallocmsg( sizeof *req );
        return NULL;
    }

    if( NULL != resp )
    {
        *resp = calloc( 1, sizeof **resp );
        if( NULL == *resp )
        {
            mallocmsg( sizeof **resp );
            free( req );
            return NULL;
        }
        (*resp)->tag = tag;
        RB_INSERT( resptree, &gl_resps, *resp );
    }

    req->id  = id;
    req->tag = tag;
    SLIST_INSERT_HEAD( &gl_reqs, req, next );

    return req;
}

int
client_quit( void )
{
    return ( NULL == addreq( IPC_MSG_QUIT, -1, NULL ) ? -1 : 0 );
}

int
client_addfiles( struct strlist * list )
{
    struct req * req;

    req = addreq( IPC_MSG_ADDFILES, -1, NULL );
    if( NULL == req )
    {
        return -1;
    }

    /* XXX need to move arg parsing back here or something */
    req->strs = list;

    return 0;
}

int
client_automap( int automap )
{
    struct req * req;

    req = addreq( IPC_MSG_AUTOMAP, -1, NULL );
    if( NULL == req )
    {
        return -1;
    }

    req->num = ( automap ? 1 : 0 );

    return 0;
}

int
client_port( int port )
{
    struct req * req;

    req = addreq( IPC_MSG_PORT, -1, NULL );
    if( NULL == req )
    {
        return -1;
    }

    req->num = port;

    return 0;
}

int
client_downlimit( int limit )
{
    struct req * req;

    req = addreq( IPC_MSG_DOWNLIMIT, -1, NULL );
    if( NULL == req )
    {
        return -1;
    }

    req->num = ( 0 > limit ? -1 : limit );

    return 0;
}

int
client_uplimit( int limit )
{
    struct req * req;

    req = addreq( IPC_MSG_UPLIMIT, -1, NULL );
    if( NULL == req )
    {
        return -1;
    }

    req->num = ( 0 > limit ? -1 : limit );

    return 0;
}

int
client_dir( const char * dir )
{
    struct req * req;
    char       * dircpy;

    dircpy = strdup( dir );
    if( NULL == dircpy )
    {
        mallocmsg( strlen( dir ) );
        return -1;
    }

    req = addreq( IPC_MSG_DIR, -1, NULL );
    if( NULL == req )
    {
        free( dircpy );
        return -1;
    }

    req->str = dircpy;

    return 0;
}

int
addintlistreq( enum ipc_msg which, size_t len, const int * list )
{
    struct req * req;
    int64_t    * duplist;
    size_t       ii;

    assert( ( 0 == len && NULL == list ) || ( 0 < len && NULL != list ) );

    duplist = NULL;
    if( NULL != list )
    {
        duplist = calloc( len, sizeof duplist[0] );
        if( NULL == duplist )
        {
            mallocmsg( len * sizeof( duplist[0] ) );
            return -1;
        }
    }

    req = addreq( which, -1, NULL );
    if( NULL == req )
    {
        free( duplist );
        return -1;
    }

    for( ii = 0; len > ii; ii++ )
    {
        duplist[ii] = list[ii];
    }
    req->listlen = len;
    req->numlist = duplist;

    return 0;
}

int
client_start( size_t len, const int * list )
{
    enum ipc_msg id;

    id = ( NULL == list ? IPC_MSG_STARTALL : IPC_MSG_START );

    return addintlistreq( id, len, list );
}

int
client_stop( size_t len, const int * list )
{
    enum ipc_msg id;

    id = ( NULL == list ? IPC_MSG_STOPALL : IPC_MSG_STOP );

    return addintlistreq( id, len, list );
}

int
client_remove( size_t len, const int * list )
{
    enum ipc_msg id;

    id = ( NULL == list ? IPC_MSG_REMOVEALL : IPC_MSG_REMOVE );

    return addintlistreq( id, len, list );
}

int
client_list( cl_listfunc func )
{
    struct req  * req;
    struct resp * resp;

    req = addreq( IPC_MSG_GETINFOALL, ++gl_tag, &resp );
    if( NULL == req )
    {
        return -1;
    }

    resp->listcb = func;
    req->types   = IPC_INF_NAME | IPC_INF_HASH;

    return 0;
}

int
client_info( cl_infofunc func )
{
    struct req  * req;
    struct resp * resp;

    req = addreq( IPC_MSG_GETINFOALL, ++gl_tag, &resp );
    if( NULL == req )
    {
        return -1;
    }

    resp->infocb = func;
    req->types   = IPC_INF_NAME | IPC_INF_HASH | IPC_INF_SIZE;

    return 0;
}

int
client_hashids( cl_hashfunc func )
{
    struct req  * req;
    struct resp * resp;

    req = addreq( IPC_MSG_GETINFOALL, ++gl_tag, &resp );
    if( NULL == req )
    {
        return -1;
    }

    resp->hashcb = func;
    req->types   = IPC_INF_HASH;

    return 0;
}

int
client_status( cl_statfunc func )
{
    struct req  * req;
    struct resp * resp;

    req = addreq( IPC_MSG_GETSTATALL, ++gl_tag, &resp );
    if( NULL == req )
    {
        return -1;
    }

    resp->statcb = func;
    req->types   = IPC_ST_STATE | IPC_ST_ETA | IPC_ST_COMPLETED |
        IPC_ST_DOWNSPEED | IPC_ST_UPSPEED | IPC_ST_ERROR | IPC_ST_ERRMSG;

    return 0;
}

void
didwrite( struct bufferevent * ev, void * arg )
{
    struct con * con = arg;

    assert( ev == con->ev );
    flushreqs( con );
}

void
ohshit( struct bufferevent * ev UNUSED, short what, void * arg UNUSED )
{
    if( EVBUFFER_EOF & what )
    {
        errmsg( "server closed connection" );
    }
    else if( EVBUFFER_TIMEOUT & what )
    {
        errmsg( "server connection timed out" );
    }
    else if( EVBUFFER_READ & what )
    {
        errmsg( "read error on server connection" );
    }
    else if( EVBUFFER_WRITE & what )
    {
        errmsg( "write error on server connection" );
    }
    else if( EVBUFFER_ERROR & what )
    {
        errmsg( "error on server connection" );
    }
    else
    {
        errmsg( "unknown error on server connection: 0x%x", what );
    }
    exit( 1 );
}

void
canread( struct bufferevent * ev, void * arg )
{
    struct con * con = arg;
    uint8_t    * buf;
    size_t       len;
    ssize_t      res;

    buf = EVBUFFER_DATA( EVBUFFER_INPUT( ev ) );
    len = EVBUFFER_LENGTH( EVBUFFER_INPUT( ev ) );

    if( IPC_MIN_MSG_LEN > len )
    {
        return;
    }

    res = ipc_parse( &con->ipc, buf, len, con );
    if( 0 > res )
    {
        switch( errno )
        {
            case EPERM:
                errmsg( "unsupported protocol version" );
                break;
            case EINVAL:
                errmsg( "protocol parse error" );
                break;
            default:
                errnomsg( "parsing failed" );
                break;
        }
        exit( 1 );
    }

    if( 0 < res )
    {
        evbuffer_drain( EVBUFFER_INPUT( ev ), res );
        flushreqs( con );
    }
}

void
flushreqs( struct con * con )
{
    struct req * req;
    uint8_t    * buf;
    size_t       buflen;

    if( !HASVERS( &con->ipc ) )
    {
        return;
    }

    if( SLIST_EMPTY( &gl_reqs ) && RB_EMPTY( &gl_resps ) )
    {
        exit( 0 );
    }

    while( !SLIST_EMPTY( &gl_reqs ) )
    {
        req = SLIST_FIRST( &gl_reqs );
        SLIST_REMOVE_HEAD( &gl_reqs, next );
        switch( req->id )
        {
            case IPC_MSG_QUIT:
            case IPC_MSG_STARTALL:
            case IPC_MSG_STOPALL:
            case IPC_MSG_REMOVEALL:
                buf = ipc_mkempty( &con->ipc, &buflen, req->id, req->tag );
                break;
            case IPC_MSG_ADDFILES:
                buf = ipc_mkstrlist( &con->ipc, &buflen, req->id, -1,
                                     req->strs );
                SAFEFREESTRLIST( req->strs );
                break;
            case IPC_MSG_AUTOMAP:
            case IPC_MSG_PORT:
            case IPC_MSG_DOWNLIMIT:
            case IPC_MSG_UPLIMIT:
                buf = ipc_mkint( &con->ipc, &buflen, req->id, -1, req->num );
                break;
            case IPC_MSG_DIR:
                buf = ipc_mkstr( &con->ipc, &buflen, req->id, -1, req->str );
                SAFEFREE( req->str );
                break;
            case IPC_MSG_START:
            case IPC_MSG_STOP:
            case IPC_MSG_REMOVE:
                buf = ipc_mkints( &con->ipc, &buflen, req->id, req->tag,
                                  req->listlen, req->numlist );
                SAFEFREE( req->numlist );
                break;
            case IPC_MSG_GETINFOALL:
            case IPC_MSG_GETSTATALL:
                buf = ipc_mkgetinfo( &con->ipc, &buflen, req->id, req->tag,
                                     req->types, NULL );
                break;
            default:
                assert( 0 );
                return;
        }

        SAFEFREE( req );
        if( NULL == buf )
        {
            if( EPERM == errno )
            {
                errmsg( "message not supported by server" );
            }
            else
            {
                errnomsg( "failed to create message" );
            }
            exit( 1 );
        }
        if( 0 > bufferevent_write( con->ev, buf, buflen ) )
        {
            errmsg( "failed to buffer %zd bytes of data for write", buflen );
            exit( 1 );
        }
        free( buf );
    }
}

int
sendvers( struct bufferevent * ev )
{
    uint8_t   * buf;
    size_t      len;

    buf = ipc_mkvers( &len );
    if( NULL == buf )
    {
        if( EPERM == errno )
        {
            errmsg( "message not supported by server" );
        }
        else
        {
            errnomsg( "failed to create message" );
        }
        return -1;
    }

    if( 0 > bufferevent_write( ev, buf, len ) )
    {
        free( buf );
        errmsg( "failed to buffer %i bytes of data for write", ( int )len );
        return -1;
    }

    free( buf );

    return 0;
}

void
infomsg( enum ipc_msg msgid, benc_val_t * list, int64_t tag,
         void * arg UNUSED )
{
    benc_val_t  * dict;
    int           ii;
    int64_t       id, size;
    char        * name, * hash;
    struct resp * resp, key;

    assert( IPC_MSG_INFO == msgid );

    if( TYPE_LIST != list->type )
    {
        return;
    }

    bzero( &key, sizeof key );
    key.tag = tag;
    resp = RB_FIND( resptree, &gl_resps, &key );
    if( NULL == resp )
    {
        return;
    }
    RB_REMOVE( resptree, &gl_resps, resp );

    for( ii = 0; list->val.l.count > ii; ii++ )
    {
        dict = &list->val.l.vals[ii];
        if( TYPE_DICT != dict->type )
        {
            continue;
        }

        id   = getinfoint( msgid, dict, IPC_INF_ID,   -1   );
        name = getinfostr( msgid, dict, IPC_INF_NAME, NULL );
        hash = getinfostr( msgid, dict, IPC_INF_HASH, NULL );
        size = getinfoint( msgid, dict, IPC_INF_SIZE, -1   );

        if( !TORRENT_ID_VALID( id ) )
        {
            continue;
        }

        if( NULL != resp->infocb )
        {
            resp->infocb( id, name, size );
        }
        else if( NULL != resp->listcb )
        {
            resp->listcb( id, name, hash );
        }
        else if( NULL != resp->hashcb )
        {
            resp->hashcb( id, hash );
        }
    }

    cbdone( resp );
    free( resp );
}

void
statmsg( enum ipc_msg msgid, benc_val_t * list, int64_t tag,
         void * arg UNUSED )
{
    benc_val_t  * dict;
    int           ii;
    int64_t       id, eta, done, down, up;
    char        * state, * error, * errmsg;
    struct resp * resp, key;

    assert( IPC_MSG_STAT == msgid );

    if( TYPE_LIST != list->type )
    {
        return;
    }

    bzero( &key, sizeof key );
    key.tag = tag;
    resp = RB_FIND( resptree, &gl_resps, &key );
    if( NULL == resp )
    {
        return;
    }
    RB_REMOVE( resptree, &gl_resps, resp );

    for( ii = 0; list->val.l.count > ii; ii++ )
    {
        dict   = &list->val.l.vals[ii];
        if( TYPE_DICT != dict->type )
        {
            continue;
        }

        id     = getinfoint( msgid, dict, IPC_ST_ID,        -1   );
        state  = getinfostr( msgid, dict, IPC_ST_STATE,     NULL );
        eta    = getinfoint( msgid, dict, IPC_ST_ETA,       -1   );
        done   = getinfoint( msgid, dict, IPC_ST_COMPLETED, -1   );
        down   = getinfoint( msgid, dict, IPC_ST_DOWNSPEED, -1   );
        up     = getinfoint( msgid, dict, IPC_ST_UPSPEED,   -1   );
        error  = getinfostr( msgid, dict, IPC_ST_ERROR,     NULL );
        errmsg = getinfostr( msgid, dict, IPC_ST_ERRMSG,    NULL );

        if( !TORRENT_ID_VALID( id ) )
        {
            continue;
        }

        if( NULL != resp->statcb )
        {
            resp->statcb( id, state, eta, done, down, up, error, errmsg );
        }
    }

    cbdone( resp );
    free( resp );
}

void
defmsg( enum ipc_msg msgid, benc_val_t * val, int64_t tag, void * arg UNUSED )
{
    struct resp * resp, key;

    switch( msgid )
    {
        case IPC_MSG_FAIL:
            if( TYPE_STR == val->type && NULL != val->val.s.s )
            {
                errmsg( "request failed: %s", val->val.s.s );
            }
            else
            {
                errmsg( "request failed" );
            }
            break;
        case IPC_MSG_NOTSUP:
            errmsg( "request message not supported" );
            break;
        default:
            break;
    }

    bzero( &key, sizeof key );
    key.tag = tag;
    resp = RB_FIND( resptree, &gl_resps, &key );
    if( NULL == resp )
    {
        return;
    }
    RB_REMOVE( resptree, &gl_resps, resp );

    cbdone( resp );
    free( resp );
}

void
cbdone( struct resp * resp )
{
    if( NULL != resp->infocb )
    {
        resp->infocb( -1, NULL, -1 );
    }
    else if( NULL != resp->listcb )
    {
        resp->listcb( -1, NULL, NULL );
    }
    else if( NULL != resp->hashcb )
    {
        resp->hashcb( -1, NULL );
    }
    else if( NULL != resp->statcb )
    {
        resp->statcb( -1, NULL, -1, -1, -1, -1, NULL, NULL );
    }
}

int64_t
getinfoint( enum ipc_msg msgid, benc_val_t * dict, int type, int64_t defval  )
{
    benc_val_t * val;

    val = tr_bencDictFind( dict, ipc_infoname( msgid, type ) );

    if( NULL != val && TYPE_INT == val->type )
    {
        return val->val.i;
    }

    return defval;
}

char *
getinfostr( enum ipc_msg msgid, benc_val_t * dict, int type, char * defval  )
{
    benc_val_t * val;

    val = tr_bencDictFind( dict, ipc_infoname( msgid, type ) );

    if( NULL != val && TYPE_STR == val->type )
    {
        return val->val.s.s ;
    }

    return defval;
}
