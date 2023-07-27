/* nbdkit
 * Copyright Red Hat
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of Red Hat nor the names of its contributors may be
 * used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY RED HAT AND CONTRIBUTORS ''AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL RED HAT OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef NBDKIT_CURLDEFS_H
#define NBDKIT_CURLDEFS_H

#include <stdbool.h>

#include "windows-compat.h"

/* Macro CURL_AT_LEAST_VERSION was added in 2015 (Curl 7.43) so if the
 * macro isn't present then Curl is very old.
 */
#ifdef CURL_AT_LEAST_VERSION
#if CURL_AT_LEAST_VERSION (7, 55, 0)
#define HAVE_CURLINFO_CONTENT_LENGTH_DOWNLOAD_T
#endif
#if CURL_AT_LEAST_VERSION (7, 61, 0)
#define HAVE_CURLINFO_NAMELOOKUP_TIME_T
#define HAVE_CURLINFO_CONNECT_TIME_T
#define HAVE_CURLINFO_APPCONNECT_TIME_T
#define HAVE_CURLINFO_PRETRANSFER_TIME_T
#define HAVE_CURLINFO_STARTTRANSFER_TIME_T
#define HAVE_CURLINFO_TOTAL_TIME_T
#define HAVE_CURLINFO_REDIRECT_TIME_T
#endif
#endif

extern const char *url;

extern unsigned connections;

extern const char *cookie_script;
extern unsigned cookie_script_renew;
extern const char *header_script;
extern unsigned header_script_renew;

extern int curl_debug_verbose;

/* The per-connection handle. */
struct handle {
  int readonly;
};

/* The libcurl handle and some associated fields and buffers. */
struct curl_handle {
  /* The underlying curl handle. */
  CURL *c;

  /* Index of this handle in the pool (for debugging). */
  size_t i;

  /* True if the handle is in use by a thread. */
  bool in_use;

  /* These fields are used/initialized when we create the handle. */
  bool accept_range;
  int64_t exportsize;

  char errbuf[CURL_ERROR_SIZE];

  /* Before doing a read or write operation, set these to point to the
   * buffer where you want the data to be stored / come from.  Note
   * the confusing terminology from libcurl: write_* is used when
   * reading, read_* is used when writing.
   */
  char *write_buf;
  uint32_t write_count;
  const char *read_buf;
  uint32_t read_count;

  /* Used by scripts.c */
  struct curl_slist *headers_copy;
};

/* config.c */
extern int curl_config (const char *key, const char *value);
extern int curl_config_complete (void);
extern const char *curl_config_help;
extern void unload_config (void);
extern struct curl_handle *allocate_handle (void);
extern void free_handle (struct curl_handle *);

/* pool.c */
extern void load_pool (void);
extern void unload_pool (void);
extern struct curl_handle *get_handle (void);
extern void put_handle (struct curl_handle *ch);

/* scripts.c */
extern int do_scripts (struct curl_handle *ch);
extern void scripts_unload (void);

/* times.c */
extern void update_times (CURL *c); /* called after every curl_easy_perform */
extern void display_times (void);

/* Translate CURLcode to nbdkit_error. */
#define display_curl_error(ch, r, fs, ...)                      \
  do {                                                          \
    nbdkit_error ((fs ": %s: %s"), ## __VA_ARGS__,              \
                  curl_easy_strerror ((r)), (ch)->errbuf);      \
  } while (0)

#endif /* NBDKIT_CURLDEFS_H */
