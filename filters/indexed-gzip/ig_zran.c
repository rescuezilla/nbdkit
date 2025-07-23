/* zran.c -- example of deflate stream indexing and random access
 * Copyright (C) 2005, 2012, 2018, 2023, 2024 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 * Version 1.6  2 Aug 2024  Mark Adler */

/* Version History:
 1.0  29 May 2005  First version
 1.1  29 Sep 2012  Fix memory reallocation error
 1.2  14 Oct 2018  Handle gzip streams with multiple members
                   Add a header file to facilitate usage in applications
 1.3  18 Feb 2023  Permit raw deflate streams as well as zlib and gzip
                   Permit crossing gzip member boundaries when extracting
                   Support a size_t size when extracting (was an int)
                   Do a binary search over the index for an access point
                   Expose the access point type to enable save and load
 1.4  13 Apr 2023  Add a NOPRIME define to not use inflatePrime()
 1.5   4 Feb 2024  Set returned index to NULL on an index build error
                   Stop decoding once request is satisfied
                   Provide a reusable inflate engine in the index
                   Allocate the dictionaries to reduce memory usage
 1.6   2 Aug 2024  Remove unneeded dependency on limits.h
 */

// Illustrate the use of Z_BLOCK, inflatePrime(), and inflateSetDictionary()
// for random access of a compressed file. A file containing a raw deflate
// stream is provided on the command line. The compressed stream is decoded in
// its entirety, and an index built with access points about every SPAN bytes
// in the uncompressed output. The compressed file is left open, and can then
// be read randomly, having to decompress on the average SPAN/2 uncompressed
// bytes before getting to the desired block of data.
//
// An access point can be created at the start of any deflate block, by saving
// the starting file offset and bit of that block, and the 32K bytes of
// uncompressed data that precede that block. Also the uncompressed offset of
// that block is saved to provide a reference for locating a desired starting
// point in the uncompressed stream. deflate_index_build() decompresses the
// input raw deflate stream a block at a time, and at the end of each block
// decides if enough uncompressed data has gone by to justify the creation of a
// new access point. If so, that point is saved in a data structure that grows
// as needed to accommodate the points.
//
// To use the index, an offset in the uncompressed data is provided, for which
// the latest access point at or preceding that offset is located in the index.
// The input file is positioned to the specified location in the index, and if
// necessary the first few bits of the compressed data is read from the file.
// inflate is initialized with those bits and the 32K of uncompressed data, and
// decompression then proceeds until the desired offset in the file is reached.
// Then decompression continues to read the requested uncompressed data from
// the file.
//
// There is some fair bit of overhead to starting inflation for the random
// access, mainly copying the 32K byte dictionary. If small pieces of the file
// are being accessed, it would make sense to implement a cache to hold some
// lookahead to avoid many calls to deflate_index_extract() for small lengths.
//
// Another way to build an index would be to use inflateCopy(). That would not
// be constrained to have access points at block boundaries, but would require
// more memory per access point, and could not be saved to a file due to the
// use of pointers in the state. The approach here allows for storage of the
// index in a file.
#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <limits.h>

#include <zlib.h>

#include <nbdkit-filter.h>

#include "cleanup.h"
#include "pread.h"
#include "minmax.h"

#include "zran.h"
#include "ig_zran.h"
#include "handle.h"

// See comments in ig_zran.h.
int ig_deflate_index_build(nbdkit_next *next, void* handle, off_t span, int* nbdkit_err) {
    struct handle *h = handle;

    nbdkit_debug("ig_deflate_index_build: starting with span=%ld, compressed_size=%lu", 
                 span, h->compressed_size);

    // Create and initialize the index list.
    struct deflate_index *index = malloc(sizeof(struct deflate_index));
    if (index == NULL)
        return Z_MEM_ERROR;
    index->have = 0;
    index->mode = 0;            // entries in index->list allocation
    index->list = NULL;
    index->strm.state = Z_NULL; // so inflateEnd() can work

    // Set up the inflation state.
    index->strm.avail_in = 0;
    index->strm.avail_out = 0;
    unsigned char buf[CHUNK];   // input buffer
    unsigned char win[WINSIZE] = {0};   // output sliding window
    off_t totin = 0;            // total bytes read from input. (Use this as our offset into the input)
    off_t totout = 0;           // total bytes uncompressed
    off_t beg = 0;              // starting offset of last history reset
    int mode = 0;               // mode: RAW, ZLIB, or GZIP (0 => not set yet)

    // Decompress from in, generating access points along the way.
    int ret;                    // the return value from zlib, or Z_ERRNO
    off_t last;                 // last access point uncompressed offset
    do {
        // Assure available input, at least until reaching EOF.
        if (index->strm.avail_in == 0) {
            // The final block is unlikely to align to the buf size, so carefully read the remainder
            size_t n = MIN (sizeof(buf), h->compressed_size - index->strm.total_in);
            nbdkit_debug("ig_deflate_index_build: reading %zu bytes at offset %lu", 
                         n, index->strm.total_in);
            // ORIGINAL:
            // index->strm.avail_in = fread(buf, 1, sizeof(buf), in);
            if (next->pread (next, buf, (uint32_t)n, index->strm.total_in, 0, nbdkit_err) == -1) {
                nbdkit_error("ig_deflate_index_build: pread failed, nbdkit_err=%d", *nbdkit_err);
                return Z_NBDKIT_ERROR;
            }
            index->strm.avail_in = n;

            totin += index->strm.avail_in;
            index->strm.next_in = buf;

            // NBDKit pread doesn't return short, unlike fread, so no need to check
            // Original:
            // if (index->strm.avail_in < sizeof(buf) && ferror(in)) {
            //    ret = Z_ERRNO;
            //    break;
            // }
                
            if (mode == 0) {
                // At the start of the input -- determine the type. Assume raw
                // if it is neither zlib nor gzip. This could in theory result
                // in a false positive for zlib, but in practice the fill bits
                // after a stored block are always zeros, so a raw stream won't
                // start with an 8 in the low nybble.
                mode = index->strm.avail_in == 0 ? RAW :    // will fail
                       (index->strm.next_in[0] & 0xf) == 8 ? ZLIB :
                       index->strm.next_in[0] == 0x1f ? GZIP :
                       /* else */ RAW;
                nbdkit_debug("ig_deflate_index_build: detected compression mode: %s (%d)", 
                             mode == RAW ? "RAW" : mode == ZLIB ? "ZLIB" : "GZIP", mode);
                index->strm.zalloc = Z_NULL;
                index->strm.zfree = Z_NULL;
                index->strm.opaque = Z_NULL;
                ret = inflateInit2(&index->strm, mode);
                if (ret != Z_OK) {
                    nbdkit_error("ig_deflate_index_build: inflateInit2 failed with ret=%d", ret);
                    break;
                }
            }
        }

        // Assure available output. This rotates the output through, for use as
        // a sliding window on the uncompressed data.
        if (index->strm.avail_out == 0) {
            index->strm.avail_out = sizeof(win);
            index->strm.next_out = win;
        }

        if (mode == RAW && index->have == 0)
            // We skip the inflate() call at the start of raw deflate data in
            // order generate an access point there. Set data_type to imitate
            // the end of a header.
            index->strm.data_type = 0x80;
        else {
            // Inflate and update the number of uncompressed bytes.
            unsigned before = index->strm.avail_out;
            ret = inflate(&index->strm, Z_BLOCK);
            totout += before - index->strm.avail_out;
        }

        if ((index->strm.data_type & 0xc0) == 0x80 &&
            (index->have == 0 || totout - last >= span)) {
            // We are at the end of a header or a non-last deflate block, so we
            // can add an access point here. Furthermore, we are either at the
            // very start for the first access point, or there has been span or
            // more uncompressed bytes since the last access point, so we want
            // to add an access point here.
            nbdkit_debug("ig_deflate_index_build: adding access point at totout=%ld, have=%d", 
                         totout, index->have);
            index = add_point(index, totin - index->strm.avail_in, totout, beg,
                              win);
            if (index == NULL) {
                nbdkit_error("ig_deflate_index_build: add_point failed");
                ret = Z_MEM_ERROR;
                break;
            }
            last = totout;
            nbdkit_debug("ig_deflate_index_build: access point added, now have=%d", index->have);
        }

        if (ret == Z_STREAM_END && mode == GZIP &&
            // Removed "|| ungetc(getc(in), in) != EOF"
            index->strm.avail_in) {
            // There is more input after the end of a gzip member. Reset the
            // inflate state to read another gzip member. On success, this will
            // set ret to Z_OK to continue decompressing.
            ret = inflateReset2(&index->strm, GZIP);
            beg = totout;           // reset history
        }

        // Keep going until Z_STREAM_END or error. If the compressed data ends
        // prematurely without a file read error, Z_BUF_ERROR is returned.
    } while (ret == Z_OK);

    if (ret != Z_STREAM_END) {
        // An error was encountered. Discard the index and return a negative
        // error code.
        nbdkit_error("ig_deflate_index_build: failed with ret=%d (expected Z_STREAM_END=%d)", 
                     ret, Z_STREAM_END);
        deflate_index_free(index);
        return ret == Z_NEED_DICT ? Z_DATA_ERROR : ret;
    }

    // Return the index.
    index->mode = mode;
    index->length = totout;
    h->index = index;
    nbdkit_debug("ig_deflate_index_build: completed successfully, have=%d, length=%ld", 
                 index->have, totout);
    return index->have;
}

// See comments in ig_zran.h.
ptrdiff_t ig_deflate_index_extract(nbdkit_next *next, void *handle, off_t offset, unsigned char *buf, size_t len, int* err) {
    struct handle *h = handle;
    struct deflate_index* index = h->index;

    nbdkit_debug("ig_deflate_index_extract: starting with offset=%ld, len=%zu", offset, len);

    // Do a quick sanity check on the index.
    if (index == NULL || index->have < 1 || index->list[0].out != 0 ||
        index->strm.state == Z_NULL) {
        nbdkit_error("ig_deflate_index_extract: sanity check failed - index=%p, have=%d, strm.state=%p", 
                     index, index ? index->have : -1, index ? index->strm.state : NULL);
        return Z_STREAM_ERROR;
    }

    // If nothing to extract, return zero bytes extracted.
    if (len == 0 || offset < 0 || offset >= index->length) {
        nbdkit_debug("ig_deflate_index_extract: nothing to extract - len=%zu, offset=%ld, index->length=%ld", 
                     len, offset, index->length);
        return 0;
    }

    // Find the access point closest to but not after offset.
    int lo = -1, hi = index->have;
    point_t *point = index->list;
    while (hi - lo > 1) {
        int mid = (lo + hi) >> 1;
        if (offset < point[mid].out)
            hi = mid;
        else
            lo = mid;
    }
    point += lo;
    
    nbdkit_debug("ig_deflate_index_extract: found access point %d - point->in=%ld, point->out=%ld, point->bits=%d", 
                 lo, point->in, point->out, point->bits);

    // Initialize the input file and prime the inflate engine to start there.
    off_t start_byte = point->in - (point->bits ? 1 : 0);
    int ch = 0;
    index->strm.avail_in = 0;
    int ret = inflateReset2(&index->strm, RAW);
    index->strm.total_in = start_byte;
    nbdkit_debug("ig_deflate_index_extract: inflateReset2 returned %d, start_byte=%ld", ret, start_byte);
    if (ret != Z_OK)
        return ret;
    if (point->bits) {
        nbdkit_debug("ig_deflate_index_extract: calling INFLATEPRIME with bits=%d", point->bits);
        INFLATEPRIME(&index->strm, point->bits, ch >> (8 - point->bits));
    }
    inflateSetDictionary(&index->strm, point->window, point->dict);
    nbdkit_debug("ig_deflate_index_extract: dictionary set, dict size=%d", point->dict);

    // Skip uncompressed bytes until offset reached, then satisfy request.
    unsigned char input[CHUNK];
    unsigned char discard[WINSIZE];
    offset -= point->out;       // number of bytes to skip to get to offset
    size_t left = len;          // number of bytes left to read after offset
    
    nbdkit_debug("ig_deflate_index_extract: adjusted offset=%ld, left=%zu", offset, left);
    
    do {
        if (offset) {
            // Discard up to offset uncompressed bytes.
            index->strm.avail_out = offset < WINSIZE ? (unsigned)offset :
                                                       WINSIZE;
            index->strm.next_out = discard;
            nbdkit_debug("ig_deflate_index_extract: discarding mode - avail_out=%u, offset=%ld", 
                         index->strm.avail_out, offset);
        }
        else {
            // Uncompress up to left bytes into buf.
            index->strm.avail_out = left < (unsigned)-1 ? (unsigned)left :
                                                          (unsigned)-1;
            index->strm.next_out = buf + len - left;
            nbdkit_debug("ig_deflate_index_extract: reading mode - avail_out=%u, left=%zu", 
                         index->strm.avail_out, left);
        }

        // Uncompress, setting got to the number of bytes uncompressed.
        if (index->strm.avail_in == 0) {
            // Assure available input.
            // In original zran.c: index->strm.avail_in = fread(input, 1, CHUNK, in);
            size_t n = MIN (sizeof(input), h->compressed_size - index->strm.total_in);
            nbdkit_debug("ig_deflate_index_extract: reading %zu bytes at total_in=%lu", 
                         n, index->strm.total_in);
            if (next->pread (next, input, (uint32_t)n, index->strm.total_in, 0, err) == -1) {
                nbdkit_debug("ig_deflate_index_extract: pread failed");
                return Z_NBDKIT_ERROR;
            }
            index->strm.avail_in = n;
            index->strm.next_in = input;
        }
        unsigned got = index->strm.avail_out;
        nbdkit_debug("ig_deflate_index_extract: before inflate - avail_in=%u, avail_out=%u, total_in=%lu, total_out=%lu", 
                     index->strm.avail_in, index->strm.avail_out, index->strm.total_in, index->strm.total_out);
        ret = inflate(&index->strm, Z_NO_FLUSH);
        got -= index->strm.avail_out;
        nbdkit_debug("ig_deflate_index_extract: after inflate - ret=%d, got=%u, avail_in=%u, avail_out=%u, total_in=%lu, total_out=%lu", 
                     ret, got, index->strm.avail_in, index->strm.avail_out, index->strm.total_in, index->strm.total_out);

        // Update the appropriate count.
        if (offset) {
            offset -= got;
        } else {
            left -= got;
            if (left == 0)
                // Request satisfied.
                break;
        }

        // If we're at the end of a gzip member and there's more to read,
        // continue to the next gzip member.
        if (ret == Z_STREAM_END && index->mode == GZIP) {
            nbdkit_debug("ig_deflate_index_extract: end of gzip member, continuing to next");
            // Discard the gzip trailer.
            unsigned drop = 8;              // length of gzip trailer
            if (index->strm.avail_in >= drop) {
                index->strm.avail_in -= drop;
                index->strm.next_in += drop;
                nbdkit_debug("ig_deflate_index_extract: discarded gzip trailer from buffer");
            }
            else {
                // Read and discard the remainder of the gzip trailer.
                drop -= index->strm.avail_in;
                index->strm.avail_in = 0;
                nbdkit_debug("ig_deflate_index_extract: need to read and discard %u more trailer bytes", drop);
                do {
                    if (h->compressed_size - index->strm.total_in == 0) {
                        // The input does not have a complete trailer.
                        // return ferror(in) ? Z_ERRNO : Z_BUF_ERROR;
                        nbdkit_debug("ig_deflate_index_extract: incomplete gzip trailer");
                        return -1;
                    }
                } while (--drop);
            }

            if (index->strm.avail_in || h->compressed_size - index->strm.total_in != 0) {
                // There's more after the gzip trailer. Use inflate to skip the
                // gzip header and resume the raw inflate there.
                nbdkit_debug("ig_deflate_index_extract: processing next gzip member");
                inflateReset2(&index->strm, GZIP);
                do {
                    if (index->strm.avail_in == 0 && index->strm.total_in < h->compressed_size) {
                        // Original zran.c:
                        // index->strm.avail_in = fread(input, 1, CHUNK, in);
                        // if (index->strm.avail_in < CHUNK && ferror(in)) {
                        //    ret = Z_ERRNO;
                        //    break;
                        //}
                        size_t n = MIN (sizeof(input), h->compressed_size - index->strm.total_in);
                        nbdkit_debug("ig_deflate_index_extract: reading %zu bytes for next gzip member at total_in=%lu", 
                                     n, index->strm.total_in);
                        if (next->pread (next, input, (uint32_t)n, index->strm.total_in, 0, err) == -1) {
                            // TODO: Improve ability to return the exact err in this circumstance
                            nbdkit_debug("ig_deflate_index_extract: pread failed for next gzip member");
                            return -1;
                        }
                        index->strm.avail_in = n;
                        index->strm.next_in = input;

                    }
                    index->strm.avail_out = WINSIZE;
                    index->strm.next_out = discard;
                    ret = inflate(&index->strm, Z_BLOCK);  // stop after header
                    nbdkit_debug("ig_deflate_index_extract: inflate Z_BLOCK returned %d, data_type=0x%x", 
                                 ret, index->strm.data_type);
                } while (ret == Z_OK && (index->strm.data_type & 0x80) == 0);
                if (ret != Z_OK) {
                    nbdkit_debug("ig_deflate_index_extract: failed to process gzip header, ret=%d", ret);
                    break;
                }
                inflateReset2(&index->strm, RAW);
                nbdkit_debug("ig_deflate_index_extract: reset to RAW mode for next gzip member");
            }
        }

        // Continue until we have the requested data, the deflate data has
        // ended, or an error is encountered.
    } while (ret == Z_OK);

    // Return the number of uncompressed bytes read into buf, or the error.
    ptrdiff_t result = ret == Z_OK || ret == Z_STREAM_END ? len - left : ret;
    nbdkit_debug("ig_deflate_index_extract: completed with result=%ld, ret=%d, len=%zu, left=%zu", 
                 result, ret, len, left);
    return result;
}
