/******************************************************************************
 * $Id$
 *
 * Copyright (c) 2005 Transmission authors and contributors
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

#ifndef TR_COMPLETION_H
#define TR_COMPLETION_H

#include "transmission.h"

tr_completion_t     * tr_cpInit( tr_torrent_t * );
void                  tr_cpClose( tr_completion_t * );
void                  tr_cpReset( tr_completion_t * );

/* General */

enum {
  CP_COMPLETE,    /* has every piece */
  CP_DONE,        /* has all the pieces but the DND ones */
  CP_INCOMPLETE   /* doesn't have all the desired pieces */
};

/* returns one of CP_SEEDING, CP_DONE, or CP_INCOMPLETE */
int tr_cpGetState ( const tr_completion_t * );

/* pass in CP_COMPLETE to see how many total bytes left,
   or CP_DONE to see how many left until you're done */
uint64_t              tr_cpLeftBytes( const tr_completion_t *, int state );

/* pass in CP_COMPLETE to see what percent of the total is complete,
   or CP_DONE to see what percent of the desired blocks are done */
float                 tr_cpCompletionAsFloat( const tr_completion_t *, int state );

/* Pieces */
int                   tr_cpPieceHasAllBlocks( const tr_completion_t *, int piece );
int                   tr_cpPieceIsComplete( const tr_completion_t *, int piece );
const tr_bitfield_t * tr_cpPieceBitfield( const tr_completion_t* );
void                  tr_cpPieceAdd( tr_completion_t *, int piece );
void                  tr_cpPieceRem( tr_completion_t *, int piece );

/* Blocks */
void                  tr_cpDownloaderAdd( tr_completion_t *, int block );
void                  tr_cpDownloaderRem( tr_completion_t *, int block );
int                   tr_cpBlockIsComplete( const tr_completion_t *, int block );
void                  tr_cpBlockAdd( tr_completion_t *, int block );
const tr_bitfield_t * tr_cpBlockBitfield( const tr_completion_t * );
void                  tr_cpBlockBitfieldSet( tr_completion_t *, tr_bitfield_t * );
float                 tr_cpPercentBlocksInPiece( const tr_completion_t * cp, int piece );
/* Missing = we don't have it and we are not getting it from any peer yet */
int                   tr_cpMissingBlocksForPiece( const tr_completion_t * cp, int piece );
int                   tr_cpMissingBlockInPiece( const tr_completion_t *, int piece );
int                   tr_cpMostMissingBlockInPiece( const tr_completion_t *, int piece,
                                                    int * downloaders );

#endif
