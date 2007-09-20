/*
 * This file Copyright (C) 2007 Charles Kerr <charles@rebelbase.com>
 *
 * This file is licensed by the GPL version 2.  Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license. 
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id$
 */

#ifndef TR_HANDSHAKE_H
#define TR_HANDSHAKE_H

#include "transmission.h"

struct tr_peerIo;
typedef struct tr_handshake tr_handshake;

typedef void (*handshakeDoneCB)(struct tr_handshake * handshake,
                                struct tr_peerIo    * io,
                                int                   isConnected,
                                const uint8_t       * peerId,
                                int                   peerSupportsEncryption,
                                void                * userData );

tr_handshake *  tr_handshakeNew( struct tr_peerIo   * io,
                                 tr_encryption_mode   encryptionMode,
                                 handshakeDoneCB      doneCB,
                                 void               * doneUserData );

void tr_handshakeAbort( tr_handshake  * handshake );

#endif
