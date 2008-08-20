/******************************************************************************
 * $Id$
 *
 * Copyright (c) 2005-2008 Transmission authors and contributors
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

#ifndef TR_UTILS_H
#define TR_UTILS_H 1

#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h> /* for size_t */
#include <stdio.h> /* FILE* */
#include <time.h> /* time_t* */

/***
****
***/

#ifndef FALSE
#define FALSE 0
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifndef UNUSED
#ifdef __GNUC__
#define UNUSED __attribute__((unused))
#else
#define UNUSED
#endif
#endif

#ifndef TR_GNUC_PRINTF
#ifdef __GNUC__
#define TR_GNUC_PRINTF( fmt, args ) __attribute__((format (printf, fmt, args)))
#else
#define TR_GNUC_PRINTF( fmt, args )
#endif
#endif

#ifndef TR_GNUC_NULL_TERMINATED
#if __GNUC__ >= 4
#define TR_GNUC_NULL_TERMINATED __attribute__((__sentinel__))
#else
#define TR_GNUC_NULL_TERMINATED
#endif
#endif

#if __GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ >= 96)
#define TR_GNUC_PURE __attribute__((__pure__))
#define TR_GNUC_MALLOC __attribute__((__malloc__))
#else
#define TR_GNUC_PURE
#define TR_GNUC_MALLOC
#endif


/***
****
***/

#if !defined(_)
#if defined(SYS_DARWIN)
  #define _(a) (a)
#elif defined(HAVE_LIBINTL_H)
  #include <libintl.h>
  #define _(a) gettext (a)
#else
  #define _(a) (a)
#endif
#endif

#define tr_nerr( n, a... ) tr_msg( __FILE__, __LINE__, TR_MSG_ERR, n, ## a )
#define tr_ninf( n, a... ) tr_msg( __FILE__, __LINE__, TR_MSG_INF, n, ## a )
#define tr_ndbg( n, a... ) tr_msg( __FILE__, __LINE__, TR_MSG_DBG, n, ## a )

#define tr_torerr( tor, a... ) tr_msg( __FILE__, __LINE__, TR_MSG_ERR, tor->info.name, ## a )
#define tr_torinf( tor, a... ) tr_msg( __FILE__, __LINE__, TR_MSG_INF, tor->info.name, ## a )
#define tr_tordbg( tor, a... ) tr_msg( __FILE__, __LINE__, TR_MSG_DBG, tor->info.name, ## a )

#define tr_err( a... ) tr_msg( __FILE__, __LINE__, TR_MSG_ERR, NULL, ## a )
#define tr_inf( a... ) tr_msg( __FILE__, __LINE__, TR_MSG_INF, NULL, ## a )
#define tr_dbg( a... ) tr_msg( __FILE__, __LINE__, TR_MSG_DBG, NULL, ## a )

void tr_msgInit( void );

void tr_msg( const char * file, int line,
             int level, const char * torrent,
             const char * fmt, ... ) TR_GNUC_PRINTF( 5, 6 );

FILE* tr_getLog( void );

void tr_deepLog( const char * file, int line,
                 const char * name,
                 const char * fmt, ... ) TR_GNUC_PRINTF( 4, 5 );

char* tr_getLogTimeStr( char * buf, int buflen );

/**
 * a portability wrapper around mkdir().
 * On WIN32, the `permissions' argument is unused.
 *
 * @return zero on success, or -1 if an error occurred
 * (in which case errno is set appropriately).
 */
int tr_mkdir( const char * path, int permissions );

/**
 * Like mkdir, but makes parent directories as needed.
 *
 * @return zero on success, or -1 if an error occurred
 * (in which case errno is set appropriately).
 */
int tr_mkdirp( const char * path, int permissions );


uint8_t* tr_loadFile( const char * filename, size_t * size ) TR_GNUC_MALLOC;


/* creates a filename from a series of elements using the
   correct separator for filenames. */
void tr_buildPath( char* buf, size_t buflen,
                   const char * first_element, ... )
                                      TR_GNUC_NULL_TERMINATED;

struct timeval tr_timevalMsec( uint64_t milliseconds );


int tr_ioErrorFromErrno( int err );

const char * tr_errorString( int code );

/* return the current date in milliseconds */
uint64_t tr_date( void );

/* wait the specified number of milliseconds */
void tr_wait( uint64_t delay_milliseconds );

/***
****
***/

/* Sometimes the system defines MAX/MIN, sometimes not. In the latter
   case, define those here since we will use them */
#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif
#ifndef MIN
#define MIN(a,b) ((a)>(b)?(b):(a))
#endif

/***
****
***/

#define tr_new(struct_type, n_structs)           \
    ((struct_type *) tr_malloc (((size_t) sizeof (struct_type)) * ((size_t) (n_structs))))
#define tr_new0(struct_type, n_structs)          \
    ((struct_type *) tr_malloc0 (((size_t) sizeof (struct_type)) * ((size_t) (n_structs))))
#define tr_renew(struct_type, mem, n_structs)    \
    ((struct_type *) realloc ((mem), ((size_t) sizeof (struct_type)) * ((size_t) (n_structs))))

void* tr_malloc  ( size_t ) TR_GNUC_MALLOC;
void* tr_malloc0 ( size_t ) TR_GNUC_MALLOC;
void* tr_calloc  ( size_t nmemb, size_t size ) TR_GNUC_MALLOC;
void  tr_free    ( void* );

char* tr_strdup( const void * str ) TR_GNUC_MALLOC;
char* tr_strndup( const void * str, int len ) TR_GNUC_MALLOC;
void* tr_memdup( const void * src, int byteCount ) TR_GNUC_MALLOC;
char* tr_strdup_printf( const char * fmt, ... )  TR_GNUC_PRINTF( 1, 2 ) TR_GNUC_MALLOC;
char* tr_base64_encode( const void * input, int inlen, int *outlen ) TR_GNUC_MALLOC;
char* tr_base64_decode( const void * input, int inlen, int *outlen ) TR_GNUC_MALLOC;

size_t tr_strlcpy( char * dst, const char * src, size_t siz );
int tr_snprintf( char * buf, size_t buflen, const char * fmt, ... );

int   tr_stringEndsWith( const char * string, const char * end );

const char* tr_strerror( int );

/***
****
***/

typedef void (tr_set_func)(void * element, void * userData );

void tr_set_compare( const void * a, size_t aCount,
                     const void * b, size_t bCount,
                     int compare( const void * a, const void * b ),
                     size_t elementSize,
                     tr_set_func in_a_cb,
                     tr_set_func in_b_cb,
                     tr_set_func in_both_cb,
                     void * userData );
                    
int tr_compareUint16( uint16_t a, uint16_t b );
int tr_compareUint32( uint32_t a, uint32_t b );
int tr_compareUint64( uint64_t a, uint64_t b );
int tr_compareDouble( double a, double b );
int tr_compareTime( time_t a, time_t b );

int tr_strcmp( const void * a, const void * b );
int tr_strcasecmp( const char * a, const char * b );

void tr_sha1_to_hex( char * out, const uint8_t * sha1 );


int tr_httpIsValidURL( const char * url );

int tr_httpParseURL( const char * url,
                     int          url_len,
                     char     ** setme_host,
                     int       * setme_port,
                     char     ** setme_path );


/***
****
***/

struct tr_bitfield
{
    uint8_t * bits;
    size_t bitCount;
    size_t byteCount;
};

typedef struct tr_bitfield tr_bitfield;
typedef struct tr_bitfield tr_bitfield_t;

tr_bitfield* tr_bitfieldNew( size_t bitcount ) TR_GNUC_MALLOC;
tr_bitfield* tr_bitfieldDup( const tr_bitfield* ) TR_GNUC_MALLOC;
void tr_bitfieldFree( tr_bitfield*);

void tr_bitfieldClear( tr_bitfield* );
int  tr_bitfieldAdd( tr_bitfield*, size_t bit );
int  tr_bitfieldRem( tr_bitfield*, size_t bit );
int  tr_bitfieldAddRange( tr_bitfield *, size_t begin, size_t end );
int  tr_bitfieldRemRange ( tr_bitfield*, size_t begin, size_t end );
void tr_bitfieldDifference( tr_bitfield *, const tr_bitfield * );

int    tr_bitfieldIsEmpty( const tr_bitfield* );
size_t tr_bitfieldCountTrueBits( const tr_bitfield* );

tr_bitfield* tr_bitfieldOr( tr_bitfield*, const tr_bitfield* );

#if 0
/** @brief finds the first true bit in the bitfield, starting at `startPos'
    @param setmePos the position of the true bit, if found, is set here.
    @return nonzero if a true bit was found */
int tr_bitfieldFindTrue( const tr_bitfield  * bitfield,
                         size_t               startPos,
                         size_t             * setmePos );
#endif


/** A stripped-down version of bitfieldHas to be used
    for speed when you're looping quickly.  This version
    has none of tr_bitfieldHas()'s safety checks, so you
    need to call tr_bitfieldTestFast() first before you
    start looping. */
#define tr_bitfieldHasFast(bitfield,nth) \
    (( bitfield->bits[(nth)>>3u] << ((nth)&7u) & 0x80) != 0 )

/** @param high the highest nth bit you're going to access */
#define tr_bitfieldTestFast(bitfield,high) \
    ( (bitfield) && ((bitfield)->bits) && ((high)<(bitfield)->bitCount ) )

#define tr_bitfieldHas(bitfield,nth) \
    ( tr_bitfieldTestFast( bitfield, nth ) && \
      tr_bitfieldHasFast( bitfield, nth ) )

double tr_getRatio( double numerator, double denominator );

#endif
