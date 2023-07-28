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

/* Curl handle pool.
 *
 * To get a libcurl handle, call get_handle().  When you hold the
 * handle, it is yours exclusively to use.  After you have finished
 * with the handle, put it back into the pool by calling put_handle().
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#include <curl/curl.h>

#include <nbdkit-plugin.h>

#include "cleanup.h"
#include "vector.h"

#include "curldefs.h"

/* Use '-D curl.pool=1' to debug handle pool. */
NBDKIT_DLL_PUBLIC int curl_debug_pool = 0;

unsigned connections = 4;

/* This lock protects access to the curl_handles vector below. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* List of curl handles.  This is allocated dynamically as more
 * handles are requested.  Currently it does not shrink.  It may grow
 * up to 'connections' in length.
 */
DEFINE_VECTOR_TYPE (curl_handle_list, struct curl_handle *);
static curl_handle_list curl_handles = empty_vector;

/* The condition is used when the curl handles vector is full and
 * we're waiting for a thread to put_handle.
 */
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static size_t in_use = 0, waiting = 0;

int
pool_get_ready (void)
{
  return 0;
}

int
pool_after_fork (void)
{
  return 0;
}

/* Close and free all handles in the pool. */
void
pool_unload (void)
{
  size_t i;

  if (curl_debug_pool)
    nbdkit_debug ("unload_pool: number of curl handles allocated: %zu",
                  curl_handles.len);

  for (i = 0; i < curl_handles.len; ++i)
    free_handle (curl_handles.ptr[i]);
  curl_handle_list_reset (&curl_handles);
}

/* Get a handle from the pool.
 *
 * It is owned exclusively by the caller until they call put_handle.
 */
struct curl_handle *
get_handle (void)
{
  ACQUIRE_LOCK_FOR_CURRENT_SCOPE (&lock);
  size_t i;
  struct curl_handle *ch;

 again:
  /* Look for a handle which is not in_use. */
  for (i = 0; i < curl_handles.len; ++i) {
    ch = curl_handles.ptr[i];
    if (!ch->in_use) {
      ch->in_use = true;
      in_use++;
      if (curl_debug_pool)
        nbdkit_debug ("get_handle: %zu", ch->i);
      return ch;
    }
  }

  /* If more connections are allowed, then allocate a new handle. */
  if (curl_handles.len < connections) {
    ch = allocate_handle ();
    if (ch == NULL)
      return NULL;
    if (curl_handle_list_append (&curl_handles, ch) == -1) {
      free_handle (ch);
      return NULL;
    }
    ch->i = curl_handles.len - 1;
    ch->in_use = true;
    in_use++;
    if (curl_debug_pool)
      nbdkit_debug ("get_handle: %zu", ch->i);
    return ch;
  }

  /* Otherwise we have run out of connections so we must wait until
   * another thread calls put_handle.
   */
  assert (in_use == connections);
  waiting++;
  while (in_use == connections)
    pthread_cond_wait (&cond, &lock);
  waiting--;

  goto again;
}

/* Return the handle to the pool. */
void
put_handle (struct curl_handle *ch)
{
  ACQUIRE_LOCK_FOR_CURRENT_SCOPE (&lock);

  if (curl_debug_pool)
    nbdkit_debug ("put_handle: %zu", ch->i);

  ch->in_use = false;
  in_use--;

  /* Signal the next thread which is waiting. */
  if (waiting > 0)
    pthread_cond_signal (&cond);
}
