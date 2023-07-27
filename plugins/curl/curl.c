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

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include <curl/curl.h>

#include <nbdkit-plugin.h>

#include "cleanup.h"

#include "curldefs.h"

const char *cookie_script = NULL;
unsigned cookie_script_renew = 0;
const char *header_script = NULL;
unsigned header_script_renew = 0;

static void
curl_load (void)
{
  CURLcode r;

  r = curl_global_init (CURL_GLOBAL_DEFAULT);
  if (r != CURLE_OK) {
    nbdkit_error ("libcurl initialization failed: %d", (int) r);
    exit (EXIT_FAILURE);
  }

  load_pool ();
}

static void
curl_unload (void)
{
  unload_config ();
  scripts_unload ();
  unload_pool ();
  display_times ();
  curl_global_cleanup ();
}

/* Create the per-connection handle. */
static void *
curl_open (int readonly)
{
  struct handle *h;

  h = calloc (1, sizeof *h);
  if (h == NULL) {
    nbdkit_error ("calloc: %m");
    return NULL;
  }
  h->readonly = readonly;

  return h;
}

/* Free up the per-connection handle. */
static void
curl_close (void *handle)
{
  struct handle *h = handle;

  free (h);
}

#define THREAD_MODEL NBDKIT_THREAD_MODEL_PARALLEL

/* Calls get_handle() ... put_handle() to get a handle for the length
 * of the current scope.
 */
#define GET_HANDLE_FOR_CURRENT_SCOPE(ch) \
  CLEANUP_PUT_HANDLE struct curl_handle *ch = get_handle ();
#define CLEANUP_PUT_HANDLE __attribute__ ((cleanup (cleanup_put_handle)))
static void
cleanup_put_handle (void *chp)
{
  struct curl_handle *ch = * (struct curl_handle **) chp;

  if (ch != NULL)
    put_handle (ch);
}

/* Get the file size. */
static int64_t
curl_get_size (void *handle)
{
  GET_HANDLE_FOR_CURRENT_SCOPE (ch);
  if (ch == NULL)
    return -1;

  return ch->exportsize;
}

/* Multi-conn is safe for read-only connections, but HTTP does not
 * have any concept of flushing so we cannot use it for read-write
 * connections.
 */
static int
curl_can_multi_conn (void *handle)
{
  struct handle *h = handle;

  return !! h->readonly;
}

/* Read data from the remote server. */
static int
curl_pread (void *handle, void *buf, uint32_t count, uint64_t offset)
{
  CURLcode r;
  char range[128];

  GET_HANDLE_FOR_CURRENT_SCOPE (ch);
  if (ch == NULL)
    return -1;

  /* Run the scripts if necessary and set headers in the handle. */
  if (do_scripts (ch) == -1) return -1;

  /* Tell the write_cb where we want the data to be written.  write_cb
   * will update this if the data comes in multiple sections.
   */
  ch->write_buf = buf;
  ch->write_count = count;

  curl_easy_setopt (ch->c, CURLOPT_HTTPGET, 1L);

  /* Make an HTTP range request. */
  snprintf (range, sizeof range, "%" PRIu64 "-%" PRIu64,
            offset, offset + count);
  curl_easy_setopt (ch->c, CURLOPT_RANGE, range);

  /* The assumption here is that curl will look after timeouts. */
  r = curl_easy_perform (ch->c);
  if (r != CURLE_OK) {
    display_curl_error (ch, r, "pread: curl_easy_perform");
    return -1;
  }
  update_times (ch->c);

  /* Could use curl_easy_getinfo here to obtain further information
   * about the connection.
   */

  /* As far as I understand the cURL API, this should never happen. */
  assert (ch->write_count == 0);

  return 0;
}

/* Write data to the remote server. */
static int
curl_pwrite (void *handle, const void *buf, uint32_t count, uint64_t offset)
{
  CURLcode r;
  char range[128];

  GET_HANDLE_FOR_CURRENT_SCOPE (ch);
  if (ch == NULL)
    return -1;

  /* Run the scripts if necessary and set headers in the handle. */
  if (do_scripts (ch) == -1) return -1;

  /* Tell the read_cb where we want the data to be read from.  read_cb
   * will update this if the data comes in multiple sections.
   */
  ch->read_buf = buf;
  ch->read_count = count;

  curl_easy_setopt (ch->c, CURLOPT_UPLOAD, 1L);

  /* Make an HTTP range request. */
  snprintf (range, sizeof range, "%" PRIu64 "-%" PRIu64,
            offset, offset + count);
  curl_easy_setopt (ch->c, CURLOPT_RANGE, range);

  /* The assumption here is that curl will look after timeouts. */
  r = curl_easy_perform (ch->c);
  if (r != CURLE_OK) {
    display_curl_error (ch, r, "pwrite: curl_easy_perform");
    return -1;
  }
  update_times (ch->c);

  /* Could use curl_easy_getinfo here to obtain further information
   * about the connection.
   */

  /* As far as I understand the cURL API, this should never happen. */
  assert (ch->read_count == 0);

  return 0;
}

static struct nbdkit_plugin plugin = {
  .name              = "curl",
  .version           = PACKAGE_VERSION,
  .load              = curl_load,
  .unload            = curl_unload,
  .config            = curl_config,
  .config_complete   = curl_config_complete,
  /* We can't set this here because of an obscure corner of the C
   * language.  "error: initializer element is not constant".  See
   * https://stackoverflow.com/questions/3025050
   */
  //.config_help       = curl_config_help,
  .magic_config_key  = "url",
  .open              = curl_open,
  .close             = curl_close,
  .get_size          = curl_get_size,
  .can_multi_conn    = curl_can_multi_conn,
  .pread             = curl_pread,
  .pwrite            = curl_pwrite,
};

static void set_help (void) __attribute__ ((constructor));
static void
set_help (void)
{
  plugin.config_help = curl_config_help;
}

NBDKIT_REGISTER_PLUGIN (plugin)
