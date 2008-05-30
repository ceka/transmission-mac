/*
 * This file Copyright (C) 2007-2008 Charles Kerr <charles@rebelbase.com>
 *
 * This file is licensed by the GPL version 2.  Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license. 
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id$
 */

#include <assert.h>
#include <string.h> /* memcpy, memcmp, strstr */
#include <stdlib.h> /* qsort */
#include <stdio.h> /* printf */
#include <limits.h> /* INT_MAX */

#include <event.h>

#include "transmission.h"
#include "blocklist.h"
#include "clients.h"
#include "completion.h"
#include "crypto.h"
#include "handshake.h"
#include "net.h"
#include "peer-io.h"
#include "peer-mgr.h"
#include "peer-mgr-private.h"
#include "peer-msgs.h"
#include "ptrarray.h"
#include "ratecontrol.h"
#include "torrent.h"
#include "trevent.h"
#include "utils.h"

enum
{
    /* how frequently to change which peers are choked */
    RECHOKE_PERIOD_MSEC = (10 * 1000),

    /* how frequently to refill peers' request lists */
    REFILL_PERIOD_MSEC = 666,

    /* following the BT spec, we consider ourselves `snubbed' if 
     * we're we don't get piece data from a peer in this long */
    SNUBBED_SEC = 60,

    /* when many peers are available, keep idle ones this long */
    MIN_UPLOAD_IDLE_SECS = (60 * 3),

    /* when few peers are available, keep idle ones this long */
    MAX_UPLOAD_IDLE_SECS = (60 * 10),

    /* how frequently to decide which peers live and die */
    RECONNECT_PERIOD_MSEC = (2 * 1000),

    /* max # of peers to ask fer per torrent per reconnect pulse */
    MAX_RECONNECTIONS_PER_PULSE = 1,

    /* max number of peers to ask for per second overall.
     * this throttle is to avoid overloading the router */
    MAX_CONNECTIONS_PER_SECOND = 8,

    /* number of unchoked peers per torrent */
    MAX_UNCHOKED_PEERS = 12,

    /* number of bad pieces a peer is allowed to send before we ban them */
    MAX_BAD_PIECES_PER_PEER = 3,

    /* use for bitwise operations w/peer_atom.myflags */
    MYFLAG_BANNED = 1,

    /* unreachable for now... but not banned.  if they try to connect to us it's okay */
    MYFLAG_UNREACHABLE = 2
};


/**
***
**/

/* We keep one of these for every peer we know about, whether
 * it's connected or not, so the struct must be small.
 * When our current connections underperform, we dip back
 * into this list for new ones. */
struct peer_atom
{   
    uint8_t from;
    uint8_t flags; /* these match the added_f flags */
    uint8_t myflags; /* flags that aren't defined in added_f */
    uint16_t port;
    uint16_t numFails;
    struct in_addr addr;
    time_t time;
    time_t piece_data_time;
};

typedef struct
{
    uint8_t hash[SHA_DIGEST_LENGTH];
    tr_ptrArray * outgoingHandshakes; /* tr_handshake */
    tr_ptrArray * pool; /* struct peer_atom */
    tr_ptrArray * peers; /* tr_peer */
    tr_timer * reconnectTimer;
    tr_timer * rechokeTimer;
    tr_timer * refillTimer;
    tr_torrent * tor;
    tr_peer * optimistic; /* the optimistic peer, or NULL if none */
    tr_bitfield * requested;

    unsigned int isRunning : 1;

    struct tr_peerMgr * manager;
}
Torrent;

struct tr_peerMgr
{
    tr_handle * handle;
    tr_ptrArray * torrents; /* Torrent */
    tr_ptrArray * incomingHandshakes; /* tr_handshake */
};

#define tordbg(t, fmt...) tr_deepLog( __FILE__, __LINE__, t->tor->info.name, ##fmt )

/**
***
**/

static void
managerLock( const struct tr_peerMgr * manager )
{
    tr_globalLock( manager->handle );
}
static void
managerUnlock( const struct tr_peerMgr * manager )
{
    tr_globalUnlock( manager->handle );
}
static void
torrentLock( Torrent * torrent )
{
    managerLock( torrent->manager );
}
static void
torrentUnlock( Torrent * torrent )
{
    managerUnlock( torrent->manager );
}
static int
torrentIsLocked( const Torrent * t )
{
    return tr_globalIsLocked( t->manager->handle );
}

/**
***
**/

static int
compareAddresses( const struct in_addr * a, const struct in_addr * b )
{
    return tr_compareUint32( a->s_addr, b->s_addr );
}

static int
handshakeCompareToAddr( const void * va, const void * vb )
{
    const tr_handshake * a = va;
    return compareAddresses( tr_handshakeGetAddr( a, NULL ), vb );
}

static int
handshakeCompare( const void * a, const void * b )
{
    return handshakeCompareToAddr( a, tr_handshakeGetAddr( b, NULL ) );
}

static tr_handshake*
getExistingHandshake( tr_ptrArray * handshakes, const struct in_addr * in_addr )
{
    return tr_ptrArrayFindSorted( handshakes,
                                  in_addr,
                                  handshakeCompareToAddr );
}

static int
comparePeerAtomToAddress( const void * va, const void * vb )
{
    const struct peer_atom * a = va;
    return compareAddresses( &a->addr, vb );
}

static int
comparePeerAtoms( const void * va, const void * vb )
{
    const struct peer_atom * b = vb;
    return comparePeerAtomToAddress( va, &b->addr );
}

/**
***
**/

static int
torrentCompare( const void * va, const void * vb )
{
    const Torrent * a = va;
    const Torrent * b = vb;
    return memcmp( a->hash, b->hash, SHA_DIGEST_LENGTH );
}

static int
torrentCompareToHash( const void * va, const void * vb )
{
    const Torrent * a = va;
    const uint8_t * b_hash = vb;
    return memcmp( a->hash, b_hash, SHA_DIGEST_LENGTH );
}

static Torrent*
getExistingTorrent( tr_peerMgr * manager, const uint8_t * hash )
{
    return (Torrent*) tr_ptrArrayFindSorted( manager->torrents,
                                             hash,
                                             torrentCompareToHash );
}

static int
peerCompare( const void * va, const void * vb )
{
    const tr_peer * a = va;
    const tr_peer * b = vb;
    return compareAddresses( &a->in_addr, &b->in_addr );
}

static int
peerCompareToAddr( const void * va, const void * vb )
{
    const tr_peer * a = va;
    return compareAddresses( &a->in_addr, vb );
}

static tr_peer*
getExistingPeer( Torrent * torrent, const struct in_addr * in_addr )
{
    assert( torrentIsLocked( torrent ) );
    assert( in_addr != NULL );

    return (tr_peer*) tr_ptrArrayFindSorted( torrent->peers,
                                             in_addr,
                                             peerCompareToAddr );
}

static struct peer_atom*
getExistingAtom( const Torrent * t, const struct in_addr * addr )
{
    assert( torrentIsLocked( t ) );
    return tr_ptrArrayFindSorted( t->pool, addr, comparePeerAtomToAddress );
}

static int
peerIsInUse( const Torrent * ct, const struct in_addr * addr )
{
    Torrent * t = (Torrent*) ct;

    assert( torrentIsLocked ( t ) );

    return getExistingPeer( t, addr )
        || getExistingHandshake( t->outgoingHandshakes, addr )
        || getExistingHandshake( t->manager->incomingHandshakes, addr );
}

static tr_peer*
peerConstructor( const struct in_addr * in_addr )
{
    tr_peer * p;
    p = tr_new0( tr_peer, 1 );
    p->rcToClient = tr_rcInit( );
    p->rcToPeer = tr_rcInit( );
    memcpy( &p->in_addr, in_addr, sizeof(struct in_addr) );
    return p;
}

static tr_peer*
getPeer( Torrent * torrent, const struct in_addr * in_addr )
{
    tr_peer * peer;

    assert( torrentIsLocked( torrent ) );

    peer = getExistingPeer( torrent, in_addr );

    if( peer == NULL ) {
        peer = peerConstructor( in_addr );
        tr_ptrArrayInsertSorted( torrent->peers, peer, peerCompare );
    }

    return peer;
}

static void
peerDestructor( tr_peer * peer )
{
    assert( peer != NULL );
    assert( peer->msgs != NULL );

    tr_peerMsgsUnsubscribe( peer->msgs, peer->msgsTag );
    tr_peerMsgsFree( peer->msgs );

    tr_peerIoFree( peer->io );

    tr_bitfieldFree( peer->have );
    tr_bitfieldFree( peer->blame );
    tr_rcClose( peer->rcToClient );
    tr_rcClose( peer->rcToPeer );
    tr_free( peer->client );
    tr_free( peer );
}

static void
removePeer( Torrent * t, tr_peer * peer )
{
    tr_peer * removed;
    struct peer_atom * atom;

    assert( torrentIsLocked( t ) );

    atom = getExistingAtom( t, &peer->in_addr );
    assert( atom != NULL );
    atom->time = time( NULL );

    removed = tr_ptrArrayRemoveSorted  ( t->peers, peer, peerCompare );
    assert( removed == peer );
    peerDestructor( removed );
}

static void
removeAllPeers( Torrent * t )
{
    while( !tr_ptrArrayEmpty( t->peers ) )
        removePeer( t, tr_ptrArrayNth( t->peers, 0 ) );
}

static void
torrentDestructor( Torrent * t )
{
    uint8_t hash[SHA_DIGEST_LENGTH];

    assert( t != NULL );
    assert( !t->isRunning );
    assert( t->peers != NULL );
    assert( torrentIsLocked( t ) );
    assert( tr_ptrArrayEmpty( t->outgoingHandshakes ) );
    assert( tr_ptrArrayEmpty( t->peers ) );

    memcpy( hash, t->hash, SHA_DIGEST_LENGTH );

    tr_timerFree( &t->reconnectTimer );
    tr_timerFree( &t->rechokeTimer );
    tr_timerFree( &t->refillTimer );

    tr_bitfieldFree( t->requested );
    tr_ptrArrayFree( t->pool, (PtrArrayForeachFunc)tr_free );
    tr_ptrArrayFree( t->outgoingHandshakes, NULL );
    tr_ptrArrayFree( t->peers, NULL );

    tr_free( t );
}

static Torrent*
torrentConstructor( tr_peerMgr * manager, tr_torrent * tor )
{
    Torrent * t;

    t = tr_new0( Torrent, 1 );
    t->manager = manager;
    t->tor = tor;
    t->pool = tr_ptrArrayNew( );
    t->peers = tr_ptrArrayNew( );
    t->outgoingHandshakes = tr_ptrArrayNew( );
    t->requested = tr_bitfieldNew( tor->blockCount );
    memcpy( t->hash, tor->info.hash, SHA_DIGEST_LENGTH );

    return t;
}

/**
 * For explanation, see http://www.bittorrent.org/fast_extensions.html
 * Also see the "test-allowed-set" unit test
 */
struct tr_bitfield *
tr_peerMgrGenerateAllowedSet( const uint32_t         k,         /* number of pieces in set */
                              const uint32_t         sz,        /* number of pieces in torrent */
                              const uint8_t        * infohash,  /* torrent's SHA1 hash*/
                              const struct in_addr * ip )       /* peer's address */
{
    uint8_t w[SHA_DIGEST_LENGTH + 4];
    uint8_t x[SHA_DIGEST_LENGTH];
    tr_bitfield_t * a;
    uint32_t a_size;

    *(uint32_t*)w = ntohl( htonl(ip->s_addr) & 0xffffff00 );   /* (1) */
    memcpy( w + 4, infohash, SHA_DIGEST_LENGTH );              /* (2) */
    tr_sha1( x, w, sizeof( w ), NULL );                        /* (3) */

    a = tr_bitfieldNew( sz );
    a_size = 0;
    
    while( a_size < k )
    {
        int i;
        for ( i=0; i<5 && a_size<k; ++i )                      /* (4) */
        {
            uint32_t j = i * 4;                                /* (5) */
            uint32_t y = ntohl(*(uint32_t*)(x+j));             /* (6) */
            uint32_t index = y % sz;                           /* (7) */
            if ( !tr_bitfieldHas( a, index ) ) {               /* (8) */
                tr_bitfieldAdd( a, index );                    /* (9) */
                ++a_size;
            }
        }
        tr_sha1( x, x, sizeof( x ), NULL );                    /* (3) */
    }
    
    return a;
}

tr_peerMgr*
tr_peerMgrNew( tr_handle * handle )
{
    tr_peerMgr * m = tr_new0( tr_peerMgr, 1 );
    m->handle = handle;
    m->torrents = tr_ptrArrayNew( );
    m->incomingHandshakes = tr_ptrArrayNew( );
    return m;
}

void
tr_peerMgrFree( tr_peerMgr * manager )
{
    managerLock( manager );

    /* free the handshakes.  Abort invokes handshakeDoneCB(), which removes
     * the item from manager->handshakes, so this is a little roundabout... */
    while( !tr_ptrArrayEmpty( manager->incomingHandshakes ) )
        tr_handshakeAbort( tr_ptrArrayNth( manager->incomingHandshakes, 0 ) );
    tr_ptrArrayFree( manager->incomingHandshakes, NULL );

    /* free the torrents. */
    tr_ptrArrayFree( manager->torrents, (PtrArrayForeachFunc)torrentDestructor );

    managerUnlock( manager );
    tr_free( manager );
}

static tr_peer**
getConnectedPeers( Torrent * t, int * setmeCount )
{
    int i, peerCount, connectionCount;
    tr_peer **peers;
    tr_peer **ret;

    assert( torrentIsLocked( t ) );

    peers = (tr_peer **) tr_ptrArrayPeek( t->peers, &peerCount );
    ret = tr_new( tr_peer*, peerCount );

    for( i=connectionCount=0; i<peerCount; ++i )
        if( peers[i]->msgs != NULL )
            ret[connectionCount++] = peers[i];

    *setmeCount = connectionCount;
    return ret;
}

static int
clientIsDownloadingFrom( const tr_peer * peer )
{
    return peer->clientIsInterested && !peer->clientIsChoked;
}

static int
clientIsUploadingTo( const tr_peer * peer )
{
    return peer->peerIsInterested && !peer->peerIsChoked;
}

/***
****
***/

int
tr_peerMgrPeerIsSeed( const tr_peerMgr       * mgr,
                      const uint8_t          * torrentHash,
                      const struct in_addr   * addr )
{
    int isSeed = FALSE;
    const Torrent * t = NULL;
    const struct peer_atom * atom = NULL;

    t = getExistingTorrent( (tr_peerMgr*)mgr, torrentHash );
    if( t )
        atom = getExistingAtom( t, addr );
    if( atom )
        isSeed = ( atom->flags & ADDED_F_SEED_FLAG ) != 0;

    return isSeed;
}

/***
****  Refill
***/

struct tr_refill_piece
{
    tr_priority_t priority;
    int missingBlockCount;
    uint16_t random;
    uint32_t piece;
    uint32_t peerCount;
};

static int
compareRefillPiece (const void * aIn, const void * bIn)
{
    const struct tr_refill_piece * a = aIn;
    const struct tr_refill_piece * b = bIn;

    /* fewer missing pieces goes first */
    if( a->missingBlockCount != b->missingBlockCount )
        return a->missingBlockCount < b->missingBlockCount ? -1 : 1;
    
    /* if one piece has a higher priority, it goes first */
    if( a->priority != b->priority )
        return a->priority > b->priority ? -1 : 1;
    
    /* otherwise if one has fewer peers, it goes first */
    if (a->peerCount != b->peerCount)
        return a->peerCount < b->peerCount ? -1 : 1;

    /* otherwise go with our random seed */
    return tr_compareUint16( a->random, b->random );
}

static int
isPieceInteresting( const tr_torrent  * tor,
                    tr_piece_index_t    piece )
{
    if( tor->info.pieces[piece].dnd ) /* we don't want it */
        return 0;

    if( tr_cpPieceIsComplete( tor->completion, piece ) ) /* we have it */
        return 0;

    return 1;
}

static uint32_t*
getPreferredPieces( Torrent     * t,
                    uint32_t    * pieceCount )
{
    const tr_torrent * tor = t->tor;
    const tr_info * inf = &tor->info;
    tr_piece_index_t i;
    uint32_t poolSize = 0;
    uint32_t * pool = tr_new( uint32_t, inf->pieceCount );
    int peerCount;
    tr_peer** peers;

    assert( torrentIsLocked( t ) );

    peers = getConnectedPeers( t, &peerCount );

    for( i=0; i<inf->pieceCount; ++i )
        if( isPieceInteresting( tor, i ) )
            pool[poolSize++] = i;

    /* sort the pool from most interesting to least... */
    if( poolSize > 1 )
    {
        uint32_t j;
        struct tr_refill_piece * p = tr_new( struct tr_refill_piece, poolSize );

        for( j=0; j<poolSize; ++j )
        {
            int k;
            const int piece = pool[j];
            struct tr_refill_piece * setme = p + j;

            setme->piece = piece;
            setme->priority = inf->pieces[piece].priority;
            setme->peerCount = 0;
            setme->random = tr_rand( UINT16_MAX );
            setme->missingBlockCount = tr_cpMissingBlocksInPiece( tor->completion, piece );

            for( k=0; k<peerCount; ++k ) {
                const tr_peer * peer = peers[k];
                if( peer->peerIsInterested && !peer->clientIsChoked && tr_bitfieldHas( peer->have, piece ) )
                    ++setme->peerCount;
            }
        }

        qsort( p, poolSize, sizeof(struct tr_refill_piece), compareRefillPiece );

        for( j=0; j<poolSize; ++j )
            pool[j] = p[j].piece;

        tr_free( p );
    }

    tr_free( peers );

    *pieceCount = poolSize;
    return pool;
}

static uint64_t*
getPreferredBlocks( Torrent * t, tr_block_index_t * setmeCount )
{
    int s;
    uint32_t i;
    uint32_t pieceCount;
    uint32_t blockCount;
    uint32_t unreqCount[3], reqCount[3];
    uint32_t * pieces;
    uint64_t * ret, * walk;
    uint64_t * unreq[3], *req[3];
    const tr_torrent * tor = t->tor;

    assert( torrentIsLocked( t ) );

    pieces = getPreferredPieces( t, &pieceCount );

    /**
     * Now we walk through those preferred pieces to find all the blocks
     * are still missing from them.  We put unrequested blocks first,
     * of course, but by including requested blocks afterwards, endgame
     * handling happens naturally.
     *
     * By doing this once per priority we also effectively get an endgame
     * mode for each priority level.  The helps keep high priority files
     * from getting stuck at 99% due of unresponsive peers.
     */

    /* make temporary bins for the four tiers of blocks */
    for( i=0; i<3; ++i ) {
        req[i] = tr_new( uint64_t, pieceCount *  tor->blockCountInPiece );
        reqCount[i] = 0;
        unreq[i] = tr_new( uint64_t, pieceCount *  tor->blockCountInPiece );
        unreqCount[i] = 0;
    }

    /* sort the blocks into our temp bins */
    for( i=blockCount=0; i<pieceCount; ++i )
    {
        const tr_piece_index_t index = pieces[i];
        const int priorityIndex = tor->info.pieces[index].priority + 1;
        const tr_block_index_t begin = tr_torPieceFirstBlock( tor, index );
        const tr_block_index_t end = begin + tr_torPieceCountBlocks( tor, index );
        tr_block_index_t block;

        for( block=begin; block<end; ++block )
        {
            if( tr_cpBlockIsComplete( tor->completion, block ) )
                continue;

            ++blockCount;

            if( tr_bitfieldHas( t->requested, block ) )
            {
                const uint32_t n = reqCount[priorityIndex]++;
                req[priorityIndex][n] = block;
            }
            else
            {
                const uint32_t n = unreqCount[priorityIndex]++;
                unreq[priorityIndex][n] = block;
            }
        }
    }

    /* join the bins together, going from highest priority to lowest so
     * the the blocks we want to request first will be first in the list */
    ret = walk = tr_new( uint64_t, blockCount );
    for( s=2; s>=0; --s ) {
        memcpy( walk, unreq[s], sizeof(uint64_t) * unreqCount[s] );
        walk += unreqCount[s];
        memcpy( walk, req[s], sizeof(uint64_t) * reqCount[s] );
        walk += reqCount[s];
    }
    assert( ( walk - ret ) == ( int )blockCount );
    *setmeCount = blockCount;

    /* cleanup */
    tr_free( pieces );
    for( i=0; i<3; ++i ) {
        tr_free( unreq[i] );
        tr_free( req[i] );
    }
    return ret;
}

static tr_peer**
getPeersUploadingToClient( Torrent * t, int * setmeCount )
{
    int i;
    int peerCount = 0;
    int retCount = 0;
    tr_peer ** peers = (tr_peer **) tr_ptrArrayPeek( t->peers, &peerCount );
    tr_peer ** ret = tr_new( tr_peer*, peerCount );

    /* get a list of peers we're downloading from */
    for( i=0; i<peerCount; ++i )
        if( clientIsDownloadingFrom( peers[i] ) )
            ret[retCount++] = peers[i];

    /* pick a different starting point each time so all peers
     * get a chance at the first blocks in the queue */
    if( retCount ) {
        tr_peer ** tmp = tr_new( tr_peer*, retCount );
        i = tr_rand( retCount );
        memcpy( tmp, ret, sizeof(tr_peer*) * retCount );
        memcpy( ret, tmp+i, sizeof(tr_peer*) * (retCount-i) );
        memcpy( ret+(retCount-i), tmp, sizeof(tr_peer*) * i );
        tr_free( tmp );
    }

    *setmeCount = retCount;
    return ret;
}

static int
refillPulse( void * vtorrent )
{
    Torrent * t = vtorrent;
    tr_torrent * tor = t->tor;
    tr_block_index_t i;
    int peerCount;
    tr_peer ** peers;
    tr_block_index_t blockCount;
    uint64_t * blocks;

    if( !t->isRunning )
        return TRUE;
    if( tr_torrentIsSeed( t->tor ) )
        return TRUE;

    torrentLock( t );
    tordbg( t, "Refilling Request Buffers..." );

    blocks = getPreferredBlocks( t, &blockCount );
    peers = getPeersUploadingToClient( t, &peerCount );

    for( i=0; peerCount && i<blockCount; ++i )
    {
        int j;

        const tr_block_index_t block = blocks[i];
        const tr_piece_index_t index = tr_torBlockPiece( tor, block );
        const uint32_t begin = (block * tor->blockSize) - (index * tor->info.pieceSize);
        const uint32_t length = tr_torBlockCountBytes( tor, block );

        assert( tr_torrentReqIsValid( tor, index, begin, length ) );
        assert( _tr_block( tor, index, begin ) == block );
        assert( begin < tr_torPieceCountBytes( tor, index ) );
        assert( (begin + length) <= tr_torPieceCountBytes( tor, index ) );

        /* find a peer who can ask for this block */
        for( j=0; j<peerCount; )
        {
            const int val = tr_peerMsgsAddRequest( peers[j]->msgs, index, begin, length );
            switch( val )
            {
                case TR_ADDREQ_FULL: 
                case TR_ADDREQ_CLIENT_CHOKED:
                    memmove( peers+j, peers+j+1, sizeof(tr_peer*)*(--peerCount-j) );
                    break;

                case TR_ADDREQ_MISSING: 
                case TR_ADDREQ_DUPLICATE: 
                    ++j;
                    break;

                case TR_ADDREQ_OK:
                    tr_bitfieldAdd( t->requested, block );
                    j = peerCount;
                    break;

                default:
                    assert( 0 && "unhandled value" );
                    break;
            }
        }
    }

    /* cleanup */
    tr_free( peers );
    tr_free( blocks );

    t->refillTimer = NULL;
    torrentUnlock( t );
    return FALSE;
}

static void
broadcastClientHave( Torrent * t, uint32_t index )
{
    int i, size;
    tr_peer ** peers;

    assert( torrentIsLocked( t ) );

    peers = getConnectedPeers( t, &size );
    for( i=0; i<size; ++i )
        tr_peerMsgsHave( peers[i]->msgs, index );
    tr_free( peers );
}

static void
broadcastGotBlock( Torrent * t, uint32_t index, uint32_t offset, uint32_t length )
{
    int i, size;
    tr_peer ** peers;

    assert( torrentIsLocked( t ) );

    peers = getConnectedPeers( t, &size );
    for( i=0; i<size; ++i )
        tr_peerMsgsCancel( peers[i]->msgs, index, offset, length );
    tr_free( peers );
}

static void
addStrike( Torrent * t, tr_peer * peer )
{
    tordbg( t, "increasing peer %s strike count to %d", tr_peerIoAddrStr(&peer->in_addr,peer->port), peer->strikes+1 );

    if( ++peer->strikes >= MAX_BAD_PIECES_PER_PEER )
    {
        struct peer_atom * atom = getExistingAtom( t, &peer->in_addr );
        atom->myflags |= MYFLAG_BANNED;
        peer->doPurge = 1;
        tordbg( t, "banning peer %s", tr_peerIoAddrStr(&atom->addr,atom->port) );
    }
}

static void
msgsCallbackFunc( void * vpeer, void * vevent, void * vt )
{
    tr_peer * peer = vpeer;
    Torrent * t = (Torrent *) vt;
    const tr_peermsgs_event * e = (const tr_peermsgs_event *) vevent;

    torrentLock( t );

    switch( e->eventType )
    {
        case TR_PEERMSG_NEED_REQ:
            if( t->refillTimer == NULL )
                t->refillTimer = tr_timerNew( t->manager->handle,
                                              refillPulse, t,
                                              REFILL_PERIOD_MSEC );
            break;

        case TR_PEERMSG_CANCEL:
            tr_bitfieldRem( t->requested, _tr_block( t->tor, e->pieceIndex, e->offset ) );
            break;

        case TR_PEERMSG_PIECE_DATA: {
            struct peer_atom * atom = getExistingAtom( t, &peer->in_addr );
            atom->piece_data_time = time( NULL );
            break;
        }

        case TR_PEERMSG_CLIENT_HAVE:
            broadcastClientHave( t, e->pieceIndex );
            tr_torrentRecheckCompleteness( t->tor );
            break;

        case TR_PEERMSG_PEER_PROGRESS: {
            struct peer_atom * atom = getExistingAtom( t, &peer->in_addr );
            const int peerIsSeed = e->progress >= 1.0;
            if( peerIsSeed ) {
                tordbg( t, "marking peer %s as a seed", tr_peerIoAddrStr(&atom->addr,atom->port) );
                atom->flags |= ADDED_F_SEED_FLAG;
            } else {
                tordbg( t, "marking peer %s as a non-seed", tr_peerIoAddrStr(&atom->addr,atom->port) );
                atom->flags &= ~ADDED_F_SEED_FLAG;
            } break;
        }

        case TR_PEERMSG_CLIENT_BLOCK:
            broadcastGotBlock( t, e->pieceIndex, e->offset, e->length );
            break;

        case TR_PEERMSG_ERROR:
            if( TR_ERROR_IS_IO( e->err ) ) {
                t->tor->error = e->err;
                tr_strlcpy( t->tor->errorString, tr_errorString( e->err ), sizeof(t->tor->errorString) );
                tr_torrentStop( t->tor );
            } else if( e->err == TR_ERROR_ASSERT ) {
                addStrike( t, peer );
            }
            peer->doPurge = 1;
            break;

        default:
            assert(0);
    }

    torrentUnlock( t );
}

static void
ensureAtomExists( Torrent * t, const struct in_addr * addr, uint16_t port, uint8_t flags, uint8_t from )
{
    if( getExistingAtom( t, addr ) == NULL )
    {
        struct peer_atom * a;
        a = tr_new0( struct peer_atom, 1 );
        a->addr = *addr;
        a->port = port;
        a->flags = flags;
        a->from = from;
        tordbg( t, "got a new atom: %s", tr_peerIoAddrStr(&a->addr,a->port) );
        tr_ptrArrayInsertSorted( t->pool, a, comparePeerAtoms );
    }
}

static int
getMaxPeerCount( const tr_torrent * tor UNUSED )
{
    return tor->maxConnectedPeers;
}

/* FIXME: this is kind of a mess. */
static void
myHandshakeDoneCB( tr_handshake    * handshake,
                   tr_peerIo       * io,
                   int               isConnected,
                   const uint8_t   * peer_id,
                   void            * vmanager )
{
    int ok = isConnected;
    uint16_t port;
    const struct in_addr * addr;
    tr_peerMgr * manager = (tr_peerMgr*) vmanager;
    Torrent * t;
    tr_handshake * ours;

    assert( io != NULL );
    assert( isConnected==0 || isConnected==1 );

    t = tr_peerIoHasTorrentHash( io )
        ? getExistingTorrent( manager, tr_peerIoGetTorrentHash( io ) )
        : NULL;

    if( tr_peerIoIsIncoming ( io ) )
        ours = tr_ptrArrayRemoveSorted( manager->incomingHandshakes,
                                        handshake, handshakeCompare );
    else if( t != NULL )
        ours = tr_ptrArrayRemoveSorted( t->outgoingHandshakes,
                                        handshake, handshakeCompare );
    else
        ours = handshake;

    assert( ours != NULL );
    assert( ours == handshake );

    if( t != NULL )
        torrentLock( t );

    addr = tr_peerIoGetAddress( io, &port );

    if( !ok || !t || !t->isRunning )
    {
        if( t ) {
            struct peer_atom * atom = getExistingAtom( t, addr );
            if( atom )
                ++atom->numFails;
        }

        tr_peerIoFree( io );
    }
    else /* looking good */
    {
        struct peer_atom * atom;
        ensureAtomExists( t, addr, port, 0, TR_PEER_FROM_INCOMING );
        atom = getExistingAtom( t, addr );

        if( atom->myflags & MYFLAG_BANNED )
        {
            tordbg( t, "banned peer %s tried to reconnect", tr_peerIoAddrStr(&atom->addr,atom->port) );
            tr_peerIoFree( io );
        }
        else if( tr_ptrArraySize( t->peers ) >= getMaxPeerCount( t->tor ) )
        {
            tr_peerIoFree( io );
        }
        else
        {
            tr_peer * peer = getExistingPeer( t, addr );

            if( peer != NULL ) /* we already have this peer */
            {
                tr_peerIoFree( io );
            }
            else
            {
                peer = getPeer( t, addr );
                tr_free( peer->client );

                if( !peer_id )
                    peer->client = NULL;
                else {
                    char client[128];
                    tr_clientForId( client, sizeof( client ), peer_id );
                    peer->client = tr_strdup( client );
                }
                peer->port = port;
                peer->io = io;
                peer->msgs = tr_peerMsgsNew( t->tor, peer, msgsCallbackFunc, t, &peer->msgsTag );
                atom->time = time( NULL );
            }
        }
    }

    if( t != NULL )
        torrentUnlock( t );
}

void
tr_peerMgrAddIncoming( tr_peerMgr      * manager,
                       struct in_addr  * addr,
                       uint16_t          port,
                       int               socket )
{
    managerLock( manager );

    if( tr_blocklistHasAddress( manager->handle, addr ) )
    {
        tr_dbg( "Banned IP address \"%s\" tried to connect to us",
                inet_ntoa( *addr ) );
        tr_netClose( socket );
    }
    else if( getExistingHandshake( manager->incomingHandshakes, addr ) )
    {
        tr_netClose( socket );
    }
    else /* we don't have a connetion to them yet... */
    {
        tr_peerIo * io;
        tr_handshake * handshake;

        io = tr_peerIoNewIncoming( manager->handle, addr, port, socket );

        handshake = tr_handshakeNew( io,
                                     manager->handle->encryptionMode,
                                     myHandshakeDoneCB,
                                     manager );

        tr_ptrArrayInsertSorted( manager->incomingHandshakes, handshake, handshakeCompare );
    }

    managerUnlock( manager );
}

void
tr_peerMgrAddPex( tr_peerMgr     * manager,
                  const uint8_t  * torrentHash,
                  uint8_t          from,
                  const tr_pex   * pex )
{
    Torrent * t;
    managerLock( manager );

    t = getExistingTorrent( manager, torrentHash );
    if( !tr_blocklistHasAddress( t->manager->handle, &pex->in_addr ) )
        ensureAtomExists( t, &pex->in_addr, pex->port, pex->flags, from );

    managerUnlock( manager );
}

tr_pex *
tr_peerMgrCompactToPex( const void  * compact,
                        size_t        compactLen,
                        const char  * added_f,
                        size_t      * pexCount )
{
    size_t i;
    size_t n = compactLen / 6;
    const uint8_t * walk = compact;
    tr_pex * pex = tr_new0( tr_pex, n );
    for( i=0; i<n; ++i ) {
        memcpy( &pex[i].in_addr, walk, 4 ); walk += 4;
        memcpy( &pex[i].port, walk, 2 ); walk += 2;
        if( added_f )
            pex[i].flags = added_f[i];
    }
    *pexCount = n;
    return pex;
}

/**
***
**/

void
tr_peerMgrSetBlame( tr_peerMgr     * manager,
                    const uint8_t  * torrentHash,
                    int              pieceIndex,
                    int              success )
{
    if( !success )
    {
        int peerCount, i;
        Torrent * t = getExistingTorrent( manager, torrentHash );
        tr_peer ** peers;

        assert( torrentIsLocked( t ) );

        peers = (tr_peer **) tr_ptrArrayPeek( t->peers, &peerCount );
        for( i=0; i<peerCount; ++i )
        {
            tr_peer * peer = peers[i];
            if( tr_bitfieldHas( peer->blame, pieceIndex ) )
            {
                tordbg( t, "peer %s contributed to corrupt piece (%d); now has %d strikes",
                           tr_peerIoAddrStr(&peer->in_addr,peer->port),
                           pieceIndex, (int)peer->strikes+1 );
                addStrike( t, peer );
            }
        }
    }
}

int
tr_pexCompare( const void * va, const void * vb )
{
    const tr_pex * a = (const tr_pex *) va;
    const tr_pex * b = (const tr_pex *) vb;
    int i = memcmp( &a->in_addr, &b->in_addr, sizeof(struct in_addr) );
    if( i ) return i;
    if( a->port < b->port ) return -1;
    if( a->port > b->port ) return 1;
    return 0;
}

int tr_pexCompare( const void * a, const void * b );

static int
peerPrefersCrypto( const tr_peer * peer )
{
    if( peer->encryption_preference == ENCRYPTION_PREFERENCE_YES )
        return TRUE;

    if( peer->encryption_preference == ENCRYPTION_PREFERENCE_NO )
        return FALSE;

    return tr_peerIoIsEncrypted( peer->io );
};

int
tr_peerMgrGetPeers( tr_peerMgr      * manager,
                    const uint8_t   * torrentHash,
                    tr_pex         ** setme_pex )
{
    const Torrent * t = getExistingTorrent( (tr_peerMgr*)manager, torrentHash );
    int i, peerCount;
    const tr_peer ** peers;
    tr_pex * pex;
    tr_pex * walk;

    managerLock( manager );

    peers = (const tr_peer **) tr_ptrArrayPeek( t->peers, &peerCount );
    pex = walk = tr_new( tr_pex, peerCount );

    for( i=0; i<peerCount; ++i, ++walk )
    {
        const tr_peer * peer = peers[i];

        walk->in_addr = peer->in_addr;

        walk->port = peer->port;

        walk->flags = 0;
        if( peerPrefersCrypto(peer) )  walk->flags |= ADDED_F_ENCRYPTION_FLAG;
        if( peer->progress >= 1.0 )    walk->flags |= ADDED_F_SEED_FLAG;
    }

    assert( ( walk - pex ) == peerCount );
    qsort( pex, peerCount, sizeof(tr_pex), tr_pexCompare );
    *setme_pex = pex;

    managerUnlock( manager );

    return peerCount;
}

static int reconnectPulse( void * vtorrent );
static int rechokePulse( void * vtorrent );

void
tr_peerMgrStartTorrent( tr_peerMgr     * manager,
                        const uint8_t  * torrentHash )
{
    Torrent * t;

    managerLock( manager );

    t = getExistingTorrent( manager, torrentHash );

    assert( t != NULL );
    assert( ( t->isRunning != 0 ) == ( t->reconnectTimer != NULL ) );
    assert( ( t->isRunning != 0 ) == ( t->rechokeTimer != NULL ) );

    if( !t->isRunning )
    {
        t->isRunning = 1;

        t->reconnectTimer = tr_timerNew( t->manager->handle,
                                         reconnectPulse, t,
                                         RECONNECT_PERIOD_MSEC );

        t->rechokeTimer = tr_timerNew( t->manager->handle,
                                       rechokePulse, t,
                                       RECHOKE_PERIOD_MSEC );

        reconnectPulse( t );

        rechokePulse( t );
    }

    managerUnlock( manager );
}

static void
stopTorrent( Torrent * t )
{
    assert( torrentIsLocked( t ) );

    t->isRunning = 0;
    tr_timerFree( &t->rechokeTimer );
    tr_timerFree( &t->reconnectTimer );

    /* disconnect the peers. */
    tr_ptrArrayForeach( t->peers, (PtrArrayForeachFunc)peerDestructor );
    tr_ptrArrayClear( t->peers );

    /* disconnect the handshakes.  handshakeAbort calls handshakeDoneCB(),
     * which removes the handshake from t->outgoingHandshakes... */
    while( !tr_ptrArrayEmpty( t->outgoingHandshakes ) )
        tr_handshakeAbort( tr_ptrArrayNth( t->outgoingHandshakes, 0 ) );
}
void
tr_peerMgrStopTorrent( tr_peerMgr     * manager,
                       const uint8_t  * torrentHash)
{
    managerLock( manager );

    stopTorrent( getExistingTorrent( manager, torrentHash ) );

    managerUnlock( manager );
}

void
tr_peerMgrAddTorrent( tr_peerMgr * manager,
                      tr_torrent * tor )
{
    Torrent * t;

    managerLock( manager );

    assert( tor != NULL );
    assert( getExistingTorrent( manager, tor->info.hash ) == NULL );

    t = torrentConstructor( manager, tor );
    tr_ptrArrayInsertSorted( manager->torrents, t, torrentCompare );

    managerUnlock( manager );
}

void
tr_peerMgrRemoveTorrent( tr_peerMgr     * manager,
                         const uint8_t  * torrentHash )
{
    Torrent * t;

    managerLock( manager );

    t = getExistingTorrent( manager, torrentHash );
    assert( t != NULL );
    stopTorrent( t );
    tr_ptrArrayRemoveSorted( manager->torrents, t, torrentCompare );
    torrentDestructor( t );

    managerUnlock( manager );
}

void
tr_peerMgrTorrentAvailability( const tr_peerMgr * manager,
                               const uint8_t    * torrentHash,
                               int8_t           * tab,
                               int                tabCount )
{
    int i;
    const Torrent * t;
    const tr_torrent * tor;
    float interval;

    managerLock( (tr_peerMgr*)manager );

    t = getExistingTorrent( (tr_peerMgr*)manager, torrentHash );
    tor = t->tor;
    interval = tor->info.pieceCount / (float)tabCount;

    memset( tab, 0, tabCount );

    for( i=0; i<tabCount; ++i )
    {
        const int piece = i * interval;

        if( tor == NULL )
            tab[i] = 0;
        else if( tr_cpPieceIsComplete( tor->completion, piece ) )
            tab[i] = -1;
        else {
            int j, peerCount;
            const tr_peer ** peers = (const tr_peer **) tr_ptrArrayPeek( t->peers, &peerCount );
            for( j=0; j<peerCount; ++j )
                if( tr_bitfieldHas( peers[j]->have, i ) )
                    ++tab[i];
        }
    }

    managerUnlock( (tr_peerMgr*)manager );
}

/* Returns the pieces that are available from peers */
tr_bitfield*
tr_peerMgrGetAvailable( const tr_peerMgr * manager,
                        const uint8_t    * torrentHash )
{
    int i, size;
    Torrent * t;
    tr_peer ** peers;
    tr_bitfield * pieces;
    managerLock( (tr_peerMgr*)manager );

    t = getExistingTorrent( (tr_peerMgr*)manager, torrentHash );
    pieces = tr_bitfieldNew( t->tor->info.pieceCount );
    peers = getConnectedPeers( t, &size );
    for( i=0; i<size; ++i )
        tr_bitfieldOr( pieces, peers[i]->have );

    managerUnlock( (tr_peerMgr*)manager );
    tr_free( peers );
    return pieces;
}

int
tr_peerMgrHasConnections( const tr_peerMgr * manager,
                          const uint8_t    * torrentHash )
{
    int ret;
    const Torrent * t;
    managerLock( (tr_peerMgr*)manager );

    t = getExistingTorrent( (tr_peerMgr*)manager, torrentHash );
    ret = t && tr_ptrArraySize( t->peers );

    managerUnlock( (tr_peerMgr*)manager );
    return ret;
}

void
tr_peerMgrTorrentStats( const tr_peerMgr * manager,
                        const uint8_t    * torrentHash,
                        int              * setmePeersKnown,
                        int              * setmePeersConnected,
                        int              * setmePeersSendingToUs,
                        int              * setmePeersGettingFromUs,
                        int              * setmePeersFrom )
{
    int i, size;
    const Torrent * t;
    const tr_peer ** peers;

    managerLock( (tr_peerMgr*)manager );

    t = getExistingTorrent( (tr_peerMgr*)manager, torrentHash );
    peers = (const tr_peer **) tr_ptrArrayPeek( t->peers, &size );

    *setmePeersKnown          = tr_ptrArraySize( t->pool );
    *setmePeersConnected      = 0;
    *setmePeersSendingToUs    = 0;
    *setmePeersGettingFromUs  = 0;

    for( i=0; i<TR_PEER_FROM__MAX; ++i )
        setmePeersFrom[i] = 0;

    for( i=0; i<size; ++i )
    {
        const tr_peer * peer = peers[i];
        const struct peer_atom * atom = getExistingAtom( t, &peer->in_addr );

        if( peer->io == NULL ) /* not connected */
            continue;

        ++*setmePeersConnected;

        ++setmePeersFrom[atom->from];

        if( clientIsDownloadingFrom( peer ) )
            ++*setmePeersSendingToUs;

        if( clientIsUploadingTo( peer ) )
            ++*setmePeersGettingFromUs;
    }

    managerUnlock( (tr_peerMgr*)manager );
}

struct tr_peer_stat *
tr_peerMgrPeerStats( const tr_peerMgr  * manager,
                     const uint8_t     * torrentHash,
                     int               * setmeCount UNUSED )
{
    int i, size;
    const Torrent * t;
    tr_peer ** peers;
    tr_peer_stat * ret;

    assert( manager != NULL );
    managerLock( (tr_peerMgr*)manager );

    t = getExistingTorrent( (tr_peerMgr*)manager, torrentHash );
    peers = getConnectedPeers( (Torrent*)t, &size );
    ret = tr_new0( tr_peer_stat, size );

    for( i=0; i<size; ++i )
    {
        char * pch;
        const tr_peer * peer = peers[i];
        const struct peer_atom * atom = getExistingAtom( t, &peer->in_addr );
        tr_peer_stat * stat = ret + i;

        tr_netNtop( &peer->in_addr, stat->addr, sizeof(stat->addr) );
        tr_strlcpy( stat->client, (peer->client ? peer->client : ""), sizeof(stat->client) );
        stat->port               = peer->port;
        stat->from               = atom->from;
        stat->progress           = peer->progress;
        stat->isEncrypted        = tr_peerIoIsEncrypted( peer->io ) ? 1 : 0;
        stat->uploadToRate       = peer->rateToPeer;
        stat->downloadFromRate   = peer->rateToClient;
        stat->peerIsChoked       = peer->peerIsChoked;
        stat->peerIsInterested   = peer->peerIsInterested;
        stat->clientIsChoked     = peer->clientIsChoked;
        stat->clientIsInterested = peer->clientIsInterested;
        stat->isIncoming         = tr_peerIoIsIncoming( peer->io );
        stat->isDownloadingFrom  = clientIsDownloadingFrom( peer );
        stat->isUploadingTo      = clientIsUploadingTo( peer );

        pch = stat->flagStr;
        if( t->optimistic == peer ) *pch++ = 'O';
        if( stat->isDownloadingFrom ) *pch++ = 'D';
        else if( stat->clientIsInterested ) *pch++ = 'd';
        if( stat->isUploadingTo ) *pch++ = 'U';
        else if( stat->peerIsInterested ) *pch++ = 'u';
        if( !stat->clientIsChoked && !stat->clientIsInterested ) *pch++ = 'K';
        if( !stat->peerIsChoked && !stat->peerIsInterested ) *pch++ = '?';
        if( stat->isEncrypted ) *pch++ = 'E';
        if( stat->from == TR_PEER_FROM_PEX ) *pch++ = 'X';
        if( stat->isIncoming ) *pch++ = 'I';
        *pch = '\0';
    }

    *setmeCount = size;
    tr_free( peers );

    managerUnlock( (tr_peerMgr*)manager );
    return ret;
}

/**
***
**/

struct ChokeData
{
    uint8_t doUnchoke;
    uint8_t isInterested;
    uint32_t rate;
    tr_peer * peer;
};

static int
compareChoke( const void * va, const void * vb )
{
    const struct ChokeData * a = va;
    const struct ChokeData * b = vb;
    return -tr_compareUint32( a->rate, b->rate );
}

static int
isNew( const tr_peer * peer )
{
    return peer && peer->io && tr_peerIoGetAge( peer->io ) < 45;
}

static int
isSame( const tr_peer * peer )
{
    return peer && peer->client && strstr( peer->client, "Transmission" );
}

/**
***
**/

static int
getWeightedRate( const tr_peer * peer, int clientIsSeed )
{
    return (int)( 10.0 * ( clientIsSeed ? peer->rateToPeer
                                        : peer->rateToClient ) );
}

static void
rechoke( Torrent * t )
{
    int i, peerCount, size, unchokedInterested;
    tr_peer ** peers = getConnectedPeers( t, &peerCount );
    struct ChokeData * choke = tr_new0( struct ChokeData, peerCount );
    const int clientIsSeed = tr_torrentIsSeed( t->tor );

    assert( torrentIsLocked( t ) );
    
    /* sort the peers by preference and rate */
    for( i=0, size=0; i<peerCount; ++i )
    {
        tr_peer * peer = peers[i];
        if( peer->progress >= 1.0 ) /* choke all seeds */
            tr_peerMsgsSetChoke( peer->msgs, TRUE );
        else {
            struct ChokeData * node = &choke[size++];
            node->peer = peer;
            node->isInterested = peer->peerIsInterested;
            node->rate = getWeightedRate( peer, clientIsSeed );
        }
    }

    qsort( choke, size, sizeof(struct ChokeData), compareChoke );

    /**
     * Reciprocation and number of uploads capping is managed by unchoking
     * the N peers which have the best upload rate and are interested.
     * This maximizes the client's download rate. These N peers are
     * referred to as downloaders, because they are interested in downloading
     * from the client.
     *
     * Peers which have a better upload rate (as compared to the downloaders)
     * but aren't interested get unchoked. If they become interested, the
     * downloader with the worst upload rate gets choked. If a client has
     * a complete file, it uses its upload rate rather than its download
     * rate to decide which peers to unchoke. 
     */
    unchokedInterested = 0;
    for( i=0; i<size && unchokedInterested<MAX_UNCHOKED_PEERS; ++i ) {
        choke[i].doUnchoke = 1;
        if( choke[i].isInterested )
            ++unchokedInterested;
    }

    /* optimistic unchoke */
    if( i < size )
    {
        struct ChokeData * c;
        tr_ptrArray * randPool = tr_ptrArrayNew( );
        for( ; i<size; ++i )
        {
            const tr_peer * peer = choke[i].peer;
            int x=1, y;
            if( isNew( peer ) ) x *= 3;
            if( isSame( peer ) ) x *= 3;
            for( y=0; y<x; ++y )
                tr_ptrArrayAppend( randPool, choke );
        }
        i = tr_rand( tr_ptrArraySize( randPool ) );
        c = tr_ptrArrayNth( randPool, i);
        c->doUnchoke = 1;
        t->optimistic = c->peer;
        tr_ptrArrayFree( randPool, NULL );
    }

    for( i=0; i<size; ++i )
        tr_peerMsgsSetChoke( choke[i].peer->msgs, !choke[i].doUnchoke );

    /* cleanup */
    tr_free( choke );
    tr_free( peers );
}

static int
rechokePulse( void * vtorrent )
{
    Torrent * t = vtorrent;
    torrentLock( t );
    rechoke( t );
    torrentUnlock( t );
    return TRUE;
}

/***
****
****  Life and Death
****
***/

static int
shouldPeerBeClosed( const Torrent * t, const tr_peer * peer, int peerCount )
{
    const tr_torrent * tor = t->tor;
    const time_t now = time( NULL );
    const struct peer_atom * atom = getExistingAtom( t, &peer->in_addr );

    /* if it's marked for purging, close it */
    if( peer->doPurge ) {
        tordbg( t, "purging peer %s because its doPurge flag is set", tr_peerIoAddrStr(&atom->addr,atom->port) );
        return TRUE;
    }

    /* if we're seeding and the peer has everything we have,
     * and enough time has passed for a pex exchange, then disconnect */
    if( tr_torrentIsSeed( tor ) ) {
        int peerHasEverything;
        if( atom->flags & ADDED_F_SEED_FLAG )
            peerHasEverything = TRUE;
        else if( peer->progress < tr_cpPercentDone( tor->completion ) )
            peerHasEverything = FALSE;
        else {
            tr_bitfield * tmp = tr_bitfieldDup( tr_cpPieceBitfield( tor->completion ) );
            tr_bitfieldDifference( tmp, peer->have );
            peerHasEverything = tr_bitfieldCountTrueBits( tmp ) == 0;
            tr_bitfieldFree( tmp );
        }
        if( peerHasEverything && ( !tr_torrentAllowsPex(tor) || (now-atom->time>=30) ) ) {
            tordbg( t, "purging peer %s because we're both seeds", tr_peerIoAddrStr(&atom->addr,atom->port) );
            return TRUE;
        }
    }

    /* disconnect if it's been too long since piece data has been transferred.
     * this is on a sliding scale based on number of available peers... */
    {
        const int relaxStrictnessIfFewerThanN = (int)((getMaxPeerCount(tor) * 0.9) + 0.5);
        /* if we have >= relaxIfFewerThan, strictness is 100%.
         * if we have zero connections, strictness is 0% */
        const float strictness = peerCount >= relaxStrictnessIfFewerThanN
            ? 1.0
            : peerCount / (float)relaxStrictnessIfFewerThanN;
        const int lo = MIN_UPLOAD_IDLE_SECS;
        const int hi = MAX_UPLOAD_IDLE_SECS;
        const int limit = lo + ((hi-lo) * strictness);
        const time_t then = peer->pieceDataActivityDate;
        const int idleTime = then ? (now-then) : 0;
        if( idleTime > limit ) {
            tordbg( t, "purging peer %s because it's been %d secs since we shared anything",
                       tr_peerIoAddrStr(&atom->addr,atom->port), idleTime );
            return TRUE;
        }
    }

    return FALSE;
}

static tr_peer **
getPeersToClose( Torrent * t, int * setmeSize )
{
    int i, peerCount, outsize;
    tr_peer ** peers = (tr_peer**) tr_ptrArrayPeek( t->peers, &peerCount );
    struct tr_peer ** ret = tr_new( tr_peer*, peerCount );

    assert( torrentIsLocked( t ) );

    for( i=outsize=0; i<peerCount; ++i )
        if( shouldPeerBeClosed( t, peers[i], peerCount ) )
            ret[outsize++] = peers[i];

    *setmeSize = outsize;
    return ret;
}

static int
compareCandidates( const void * va, const void * vb )
{
    const struct peer_atom * a = * (const struct peer_atom**) va;
    const struct peer_atom * b = * (const struct peer_atom**) vb;
    int i;

    if( a->piece_data_time > b->piece_data_time ) return -1;
    if( a->piece_data_time < b->piece_data_time ) return  1;

    if(( i = tr_compareUint16( a->numFails, b->numFails )))
        return i;

    if( a->time != b->time )
        return a->time < b->time ? -1 : 1;

    return 0;
}

static struct peer_atom **
getPeerCandidates( Torrent * t, int * setmeSize )
{
    int i, atomCount, retCount;
    struct peer_atom ** atoms;
    struct peer_atom ** ret;
    const time_t now = time( NULL );
    const int seed = tr_torrentIsSeed( t->tor );

    assert( torrentIsLocked( t ) );

    atoms = (struct peer_atom**) tr_ptrArrayPeek( t->pool, &atomCount );
    ret = tr_new( struct peer_atom*, atomCount );
    for( i=retCount=0; i<atomCount; ++i )
    {
        struct peer_atom * atom = atoms[i];

        /* peer fed us too much bad data ... we only keep it around
         * now to weed it out in case someone sends it to us via pex */
        if( atom->myflags & MYFLAG_BANNED )
            continue;

        /* peer was unconnectable before, so we're not going to keep trying.
         * this is needs a separate flag from `banned', since if they try
         * to connect to us later, we'll let them in */
        if( atom->myflags & MYFLAG_UNREACHABLE )
            continue;

        /* we don't need two connections to the same peer... */
        if( peerIsInUse( t, &atom->addr ) )
            continue;

        /* no need to connect if we're both seeds... */
        if( seed && (atom->flags & ADDED_F_SEED_FLAG) )
            continue;

        /* we're wasting our time trying to connect to this bozo. */
        if( atom->numFails > 3 )
            continue;

        /* If we were connected to this peer recently and transferring
         * piece data, try to reconnect -- network troubles may have
         * disconnected us.  but if we weren't sharing piece data,
         * hold off on this peer to give another one a try instead */
        if( ( now - atom->piece_data_time ) > 30 )
        {
            int minWait = (60 * 10); /* ten minutes */
            int maxWait = (60 * 30); /* thirty minutes */
            int wait = atom->numFails * minWait;
            if( wait < minWait ) wait = minWait;
            if( wait > maxWait ) wait = maxWait;
            if( ( now - atom->time ) < wait ) {
                tordbg( t, "RECONNECT peer %d (%s) is in its grace period of %d seconds..",
                        i, tr_peerIoAddrStr(&atom->addr,atom->port), wait );
                continue;
            }
        }

        /* Don't connect to peers in our blocklist */
        if( tr_blocklistHasAddress( t->manager->handle, &atom->addr ) )
            continue;

        ret[retCount++] = atom;
    }

    qsort( ret, retCount, sizeof(struct peer_atom*), compareCandidates );
    *setmeSize = retCount;
    return ret;
}

static int
reconnectPulse( void * vtorrent )
{
    Torrent * t = vtorrent;
    static time_t prevTime = 0;
    static int newConnectionsThisSecond = 0;
    time_t now;

    torrentLock( t );

    now = time( NULL );
    if( prevTime != now )
    {
        prevTime = now;
        newConnectionsThisSecond = 0;
    }

    if( !t->isRunning )
    {
        removeAllPeers( t );
    }
    else
    {
        int i, nCandidates, nBad;
        struct peer_atom ** candidates = getPeerCandidates( t, &nCandidates );
        struct tr_peer ** connections = getPeersToClose( t, &nBad );

        if( nBad || nCandidates )
            tordbg( t, "reconnect pulse for [%s]: %d bad connections, "
                       "%d connection candidates, %d atoms, max per pulse is %d",
                       t->tor->info.name, nBad, nCandidates,
                       tr_ptrArraySize(t->pool),
                       (int)MAX_RECONNECTIONS_PER_PULSE );

        /* disconnect some peers.
           if we got transferred piece data, then they might be good peers,
           so reset their `numFails' weight to zero.  otherwise we connected
           to them fruitlessly, so mark it as another fail */
        for( i=0; i<nBad; ++i ) {
            tr_peer * peer = connections[i];
            struct peer_atom * atom = getExistingAtom( t, &peer->in_addr );
            if( peer->pieceDataActivityDate )
                atom->numFails = 0;
            else
                ++atom->numFails;
            removePeer( t, peer );
        }

        /* add some new ones */
        for( i=0;    i < nCandidates
                  && i < MAX_RECONNECTIONS_PER_PULSE
                  && newConnectionsThisSecond < MAX_CONNECTIONS_PER_SECOND; ++i )
        {
            tr_peerMgr * mgr = t->manager;
            struct peer_atom * atom = candidates[i];
            tr_peerIo * io;

            tordbg( t, "Starting an OUTGOING connection with %s",
                       tr_peerIoAddrStr( &atom->addr, atom->port ) );

            io = tr_peerIoNewOutgoing( mgr->handle, &atom->addr, atom->port, t->hash );
            if( io == NULL )
            {
                atom->myflags |= MYFLAG_UNREACHABLE;
            }
            else
            {
                tr_handshake * handshake = tr_handshakeNew( io,
                                                            mgr->handle->encryptionMode,
                                                            myHandshakeDoneCB,
                                                            mgr );

                assert( tr_peerIoGetTorrentHash( io ) != NULL );

                ++newConnectionsThisSecond;

                tr_ptrArrayInsertSorted( t->outgoingHandshakes, handshake, handshakeCompare );
            }

            atom->time = time( NULL );
        }

        /* cleanup */
        tr_free( connections );
        tr_free( candidates );
    }

    torrentUnlock( t );
    return TRUE;
}
