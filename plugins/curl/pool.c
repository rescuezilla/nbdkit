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

/* Worker thread which processes the curl multi interface.
 *
 * The main nbdkit threads (see curl.c) create curl easy handles
 * initialized with the work they want to carry out.  Note there is
 * one easy handle per task (eg. per pread/pwrite request).  The easy
 * handles are not reused.
 *
 * The commands + optional easy handle are submitted to the worker
 * thread over a self-pipe (it's easy to use a pipe here because the
 * way curl multi works is it can listen on an extra fd, but not on
 * anything else like a pthread condition).  The curl multi performs
 * the work of the outstanding easy handles.
 *
 * When an easy handle finishes work or errors, we retire the command
 * by signalling back to the waiting nbdkit thread using a pthread
 * condition.
 *
 * In my experiments, we're almost always I/O bound so I haven't seen
 * any strong need to use more than one curl multi / worker thread,
 * although it would be possible to add more in future.
 *
 * See also this extremely useful thread:
 * https://curl.se/mail/lib-2019-03/0100.html
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>

#include <curl/curl.h>

#include <nbdkit-plugin.h>

#include "cleanup.h"
#include "vector.h"

#include "curldefs.h"

/* Use '-D curl.pool=1' to debug handle pool. */
NBDKIT_DLL_PUBLIC int curl_debug_pool = 0;

unsigned connections = 16;

/* Pipe used to notify background thread that a command is pending in
 * the queue.  A pointer to the 'struct command' is sent over the
 * pipe.
 */
static int self_pipe[2] = { -1, -1 };

/* The curl multi handle. */
static CURLM *multi;

/* List of running easy handles.  We only need to maintain this so we
 * can remove them from the multi handle when cleaning up.
 */
DEFINE_VECTOR_TYPE (curl_handle_list, struct curl_handle *);
static curl_handle_list curl_handles = empty_vector;

static const char *
command_type_to_string (enum command_type type)
{
  switch (type) {
  case EASY_HANDLE: return "EASY_HANDLE";
  case STOP:        return "STOP";
  default:          abort ();
  }
}

int
pool_get_ready (void)
{
  multi = curl_multi_init ();
  if (multi == NULL) {
    nbdkit_error ("curl_multi_init failed: %m");
    return -1;
  }

#ifdef HAVE_CURLMOPT_MAX_TOTAL_CONNECTIONS
  curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long) connections);
#endif

  return 0;
}

/* Start and stop the background thread. */
static pthread_t thread;
static bool thread_running;
static void *pool_worker (void *);

int
pool_after_fork (void)
{
  int err;

  if (pipe (self_pipe) == -1) {
    nbdkit_error ("pipe: %m");
    return -1;
  }

  /* Start the pool background thread where all the curl work is done. */
  err = pthread_create (&thread, NULL, pool_worker, NULL);
  if (err != 0) {
    errno = err;
    nbdkit_error ("pthread_create: %m");
    return -1;
  }
  thread_running = true;

  return 0;
}

/* Unload the background thread. */
void
pool_unload (void)
{
  if (thread_running) {
    /* Stop the background thread. */
    struct command cmd = { .type = STOP };
    send_command_and_wait (&cmd);
    pthread_join (thread, NULL);
    thread_running = false;
  }

  if (self_pipe[0] >= 0) {
    close (self_pipe[0]);
    self_pipe[0] = -1;
  }
  if (self_pipe[1] >= 0) {
    close (self_pipe[1]);
    self_pipe[1] = -1;
  }

  if (multi) {
    size_t i;

    /* Remove and free any easy handles in the multi. */
    for (i = 0; i < curl_handles.len; ++i) {
      curl_multi_remove_handle (multi, curl_handles.ptr[i]->c);
      free_handle (curl_handles.ptr[i]);
    }

    curl_multi_cleanup (multi);
    multi = NULL;
  }
}

/* Command queue. */
static _Atomic uint64_t id;     /* next command ID */

/* Send command to the background thread and wait for completion.
 * This is only called by one of the nbdkit threads.
 */
CURLcode
send_command_and_wait (struct command *cmd)
{
  cmd->id = id++;

  /* CURLcode is 0 (CURLE_OK) or > 0, so use -1 as a sentinel to
   * indicate that the command has not yet been completed and status
   * set.
   */
  cmd->status = -1;

  /* This will be used to signal command completion back to us. */
  pthread_mutex_init (&cmd->mutex, NULL);
  pthread_cond_init (&cmd->cond, NULL);

  /* Send the command to the background thread. */
  if (write (self_pipe[1], &cmd, sizeof cmd) != sizeof cmd)
    abort ();

  /* Wait for the command to be completed by the background thread. */
  {
    ACQUIRE_LOCK_FOR_CURRENT_SCOPE (&cmd->mutex);
    while (cmd->status == -1) /* for -1, see above */
      pthread_cond_wait (&cmd->cond, &cmd->mutex);
  }

  pthread_mutex_destroy (&cmd->mutex);
  pthread_cond_destroy (&cmd->cond);

  /* Note the main thread must call nbdkit_error on error! */
  return cmd->status;
}

/* The background thread. */
static struct command *process_multi_handle (void);
static void check_for_finished_handles (void);
static void retire_command (struct command *cmd, CURLcode code);
static void do_easy_handle (struct command *cmd);

static void *
pool_worker (void *vp)
{
  bool stop = false;

  if (curl_debug_pool)
    nbdkit_debug ("curl: background thread started");

  while (!stop) {
    struct command *cmd = NULL;

    cmd = process_multi_handle ();
    if (cmd == NULL)
      continue; /* or die?? */

    if (curl_debug_pool)
      nbdkit_debug ("curl: dispatching %s command %" PRIu64,
                    command_type_to_string (cmd->type), cmd->id);

    switch (cmd->type) {
    case STOP:
      stop = true;
      retire_command (cmd, CURLE_OK);
      break;

    case EASY_HANDLE:
      do_easy_handle (cmd);
      break;
    }
  } /* while (!stop) */

  if (curl_debug_pool)
    nbdkit_debug ("curl: background thread stopped");

  return NULL;
}

/* Process the multi handle, and look out for new commands.  Returns
 * when there is a new command.
 */
static struct command *
process_multi_handle (void)
{
  struct curl_waitfd extra_fds[1] =
  { { .fd = self_pipe[0], .events = CURL_WAIT_POLLIN } };
  CURLMcode mc;
  int numfds, running_handles;
  struct command *cmd = NULL;
#ifndef HAVE_CURL_MULTI_POLL
  int repeats = 0;
#endif

  while (!cmd) {
    /* Process the multi handle. */
    mc = curl_multi_perform (multi, &running_handles);
    if (mc != CURLM_OK) {
      nbdkit_error ("curl_multi_perform: %s", curl_multi_strerror (mc));
      return NULL;
    }

    check_for_finished_handles ();

#ifdef HAVE_CURL_MULTI_POLL
    mc = curl_multi_poll (multi, extra_fds, 1, 1000000, &numfds);
    if (mc != CURLM_OK) {
      nbdkit_error ("curl_multi_poll: %s", curl_multi_strerror (mc));
      return NULL;
    }
#else
    /* This is the older curl_multi_wait function.  For unclear
     * reasons this often gets "stuck" in the nbdkit_nanosleep case
     * below, wasting large amounts of time.  Luckily the newer curl
     * no longer uses this function.
     */
    mc = curl_multi_wait (multi, extra_fds, 1, 1000000, &numfds);
    if (mc != CURLM_OK) {
      nbdkit_error ("curl_multi_wait: %s", curl_multi_strerror (mc));
      return NULL;
    }

    if (numfds == 0) {
      repeats++;
      if (repeats > 1)
        nbdkit_nanosleep (1, 0);
      continue;
    }
    else
      repeats = 0;
#endif

    if (curl_debug_pool)
      nbdkit_debug (
#ifdef HAVE_CURL_MULTI_POLL
                    "curl_multi_poll"
#else
                    "curl_multi_wait"
#endif
                    ": running_handles=%d numfds=%d",
                    running_handles, numfds);

    if (extra_fds[0].revents == CURL_WAIT_POLLIN) {
      /* There's a command waiting. */
      if (read (self_pipe[0], &cmd, sizeof cmd) != sizeof cmd)
        abort ();
    }
  }

  return cmd;
}

/* This checks if any easy handles in the multi have
 * finished and retires the associated commands.
 */
static void
check_for_finished_handles (void)
{
  CURLMsg *msg;
  int msgs_in_queue;

  while ((msg = curl_multi_info_read (multi, &msgs_in_queue)) != NULL) {
    size_t i;
    struct curl_handle *ch = NULL;

    if (msg->msg == CURLMSG_DONE) {
      /* Find this curl_handle. */
      for (i = 0; i < curl_handles.len; ++i) {
        if (curl_handles.ptr[i]->c == msg->easy_handle) {
          ch = curl_handles.ptr[i];
          curl_handle_list_remove (&curl_handles, i);
          break;
        }
      }
      if (ch == NULL) abort ();
      curl_multi_remove_handle (multi, ch->c);

      retire_command (ch->cmd, msg->data.result);
    }
  }
}

/* Retire a command.  status is a CURLcode. */
static void
retire_command (struct command *cmd, CURLcode status)
{
  if (curl_debug_pool)
    nbdkit_debug ("curl: retiring %s command %" PRIu64,
                  command_type_to_string (cmd->type), cmd->id);

  ACQUIRE_LOCK_FOR_CURRENT_SCOPE (&cmd->mutex);
  cmd->status = status;
  pthread_cond_signal (&cmd->cond);
}

static void
do_easy_handle (struct command *cmd)
{
  CURLMcode mc;

  cmd->ch->cmd = cmd;

  /* Add the handle to the multi. */
  mc = curl_multi_add_handle (multi, cmd->ch->c);
  if (mc != CURLM_OK) {
    nbdkit_error ("curl_multi_add_handle: %s", curl_multi_strerror (mc));
    goto err;
  }

  if (curl_handle_list_append (&curl_handles, cmd->ch) == -1)
    goto err;
  return;

 err:
  retire_command (cmd, CURLE_OUT_OF_MEMORY);
}
