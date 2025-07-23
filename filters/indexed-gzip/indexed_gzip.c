#include <config.h>
#include <errno.h>

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

#include "ig_zran.h"

#define FILTER_NAME "indexed-gzip"

#define DEFAULT_SPAN_HELP_STRING "1MB"
#define DEFAULT_SPAN_IN_BYTES (1024 * 1024)

/* NBDKit parameters populated by parsing the the command line. */
static char *gzip_index_path = NULL;
static off_t span_in_bytes = DEFAULT_SPAN_IN_BYTES;

// The handle only contains one z_stream zlib decompression structure, and read operations modify this structure
// Therefore, cannot yet support multiple threads safely.
// FIXME: Remove this limitation and drop this lock
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* Called for each key=value passed on the command line. */
static int
indexed_gzip_config (nbdkit_next_config *next, nbdkit_backend *nxdata,
                 const char *key, const char *value)
{
  if (strcmp (key, "gzip-index-path") == 0) {
    gzip_index_path = strdup(value);
    return 0;
  }
  else if (strcmp (key, "span") == 0) {
    int64_t r = nbdkit_parse_size (value);
    if (r == -1)
      return -1;
    if (r < 0) {
      nbdkit_error ("span cannot be negative");
      return -1;
    }
    span_in_bytes = r;
    nbdkit_debug("Custom span set to %" PRIi64, span_in_bytes);
    return 0;
  }
  else
    return next (nxdata, key, value);
}

#define indexed_gzip_config_help \
  "gzip-index-path=<PATH>                Path to the complete gzip index file (created if it doesn't exist, reused if it does).\n" \
  "span=<SIZE>                           Number of bytes between index points. Eg. 1M, 10M etc. (default: " DEFAULT_SPAN_HELP_STRING ")\n" \
  "                                          A span of 10M produces an index file of eg, ~0.3% of uncompressed input\n" \
  "                                          A span of 1M produces an index file of eg, ~3% of uncompressed input.\n" \
  "                                          Smaller span improves random-access performance since on average it means fewer bytes to\n" \
  "                                          decompress until reaching the requested byte (with the trade-off of a larger index file).\n" \

static void *
indexed_gzip_open(nbdkit_next_open *next, nbdkit_context *nxdata,
                             int readonly, const char *exportname, int is_tls)
{
    struct handle *h;

    /* Always pass readonly=1 to the underlying plugin. */
    if (next(nxdata, 1, exportname) == -1)
        return NULL;

    h = malloc(sizeof *h);
    if (h == NULL) {
        nbdkit_error("malloc: %m");
        return NULL;
    }

    h->index = NULL;
    h->compressed_size = 0;

    return h;
}

static void
indexed_gzip_close(void *handle) {
    struct handle *h = handle;
    free(h);

    return;
}

static int
indexed_gzip_prepare(nbdkit_next *next, void *handle, int readonly)
{
    struct handle *h = handle;

    h->compressed_size = next->get_size(next);

    /* Load index for this handle */
    FILE* input_file_fp = fopen(gzip_index_path, "rb");
    if (input_file_fp == NULL) {
        // It's expected that the file does not exist upon first run
        nbdkit_debug("Cannot open provided index file: %s\nCreating new index", gzip_index_path);
        int err = 0;
        int len = ig_deflate_index_build(next, handle, span_in_bytes, &err);
        if (len < 0) {
            switch (len) {
            case Z_NBDKIT_ERROR:
                 // err has been set by the caller
                 nbdkit_error(FILTER_NAME ": nbdkit error has occured\n");
                 break;
            case Z_MEM_ERROR:
                err = EIO;
                nbdkit_error(FILTER_NAME ": out of memory\n");
                break;
            case Z_BUF_ERROR:
                err = EIO;
                nbdkit_error(FILTER_NAME ": %s ended prematurely\n", gzip_index_path);
                break;
            case Z_DATA_ERROR:
                err = EIO;
                nbdkit_error(FILTER_NAME ": compressed data error in %s\n", gzip_index_path);
                break;
            case Z_ERRNO:
                err = EIO;
                nbdkit_error(FILTER_NAME ": read error on %s\n", gzip_index_path);
                break;
            default:
                err = EIO;
                nbdkit_error(FILTER_NAME ": error %d while building index\n", len);
            }
            return -1;
        }
        nbdkit_debug(FILTER_NAME ": built index with %d access points\n", len);

        // Write out newly generated index
        FILE* new_index_file_fp = fopen(gzip_index_path, "wb");
        if (deflate_index_serialize(h->index, new_index_file_fp) < 0) {
            nbdkit_error(FILTER_NAME ": failed to write the index with %d access points to file\n", len);
            fclose(new_index_file_fp);
            return -1;
        }
        fclose(new_index_file_fp);
    } else {
        nbdkit_debug("Trying existing index file: %s: %m", gzip_index_path);
        h->index = deflate_index_deserialize(input_file_fp);
        fclose(input_file_fp);
        if (h->index == NULL) {
            nbdkit_error("Failed to load index from %s\n", gzip_index_path);
            return -1;
        }
        nbdkit_debug("Loaded index from %s\n", gzip_index_path);
    }

    nbdkit_debug("Indexed gzip prepare completed successfully. Index has %d access points", h->index->have);
    return 0;
}


static int
indexed_gzip_finalize(nbdkit_next *next, void *handle)
{
    struct handle *h = handle;

    /* Free the index if it was loaded for this handle */
    if (h->index != NULL) {
        deflate_index_free(h->index);
        h->index = NULL;
    }

    return 0;
}

/* Whatever the plugin says, this filter makes it read-only. */
static int
indexed_gzip_can_write(nbdkit_next *next, void *handle)
{
    return 0;
}

/* Whatever the plugin says, this filter is consistent across connections. */
static int
indexed_gzip_can_multi_conn(nbdkit_next *next, void *handle)
{
    return 1;
}

/* Similar to above, whatever the plugin says, extents are not supported. */
static int
indexed_gzip_can_extents(nbdkit_next *next, void *handle)
{
    return 0;
}

/* We are already operating as a cache regardless of the plugin's underlying .can_cache. */
static int
indexed_gzip_can_cache(nbdkit_next *next, void *handle)
{
    return NBDKIT_CACHE_EMULATE;
}

/* Description. */
static const char *
indexed_gzip_export_description(nbdkit_next *next, void *handle)
{
    const char *base = next->export_description(next);

    if (!base)
        return NULL;
    return nbdkit_printf_intern("indexed gzip decompression: %s", base);
}

/* Get the file size. */
static int64_t
indexed_gzip_get_size(nbdkit_next *next, void *handle)
{
    struct handle *h = handle;

    int64_t uncompressed_size = (int64_t)h->index->length;

    if (uncompressed_size < 0) {
        nbdkit_error(FILTER_NAME ": ERROR: uncompressed_file_size %llu is too large for int64_t! Exceeds INT64_MAX=%lld",
                     (unsigned long long)h->index->length, (long long)INT64_MAX);
        return -1;
    }

    return uncompressed_size;
}

/* Read data using the index. */
static int
indexed_gzip_pread(nbdkit_next *next, void *handle,
                              void *buf, uint32_t count, uint64_t offset,
                              uint32_t flags, int *err)
{
    ACQUIRE_LOCK_FOR_CURRENT_SCOPE (&lock);

    struct handle *h = handle;

    if (h->index == NULL) {
        nbdkit_error(FILTER_NAME ": index not loaded");
        *err = EIO;
        return -1;
    }

    ptrdiff_t len = ig_deflate_index_extract(next, h, offset, buf, count, err);
    if (len < 0) {
        switch (len) {
        case Z_NBDKIT_ERROR:
             // err has been set by the caller
             nbdkit_error(FILTER_NAME ": nbdkit error has occured\n");
             break;
        default:
            //
            *err = EIO;
            nbdkit_error(FILTER_NAME ": error %ld while extracting value\n", len);
        }
        return -1;
    }
    return 0;
}

static struct nbdkit_filter filter = {
    .name               = FILTER_NAME,
    .longname           = "nbdkit indexed gzip filter",
    .config             = indexed_gzip_config,
    .config_help        = indexed_gzip_config_help,
    .open               = indexed_gzip_open,
    .prepare            = indexed_gzip_prepare,
    .finalize           = indexed_gzip_finalize,
    .close              = indexed_gzip_close,
    .can_write          = indexed_gzip_can_write,
    .can_extents        = indexed_gzip_can_extents,
    .can_cache          = indexed_gzip_can_cache,
    .can_multi_conn     = indexed_gzip_can_multi_conn,
    .export_description = indexed_gzip_export_description,
    .get_size           = indexed_gzip_get_size,
    .pread              = indexed_gzip_pread,
};

NBDKIT_REGISTER_FILTER(filter)
