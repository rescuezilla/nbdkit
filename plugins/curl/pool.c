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

#include "ascii-ctype.h"
#include "ascii-string.h"
#include "cleanup.h"
#include "vector.h"

#include "curldefs.h"

/* Use '-D curl.pool=1' to debug handle pool. */
NBDKIT_DLL_PUBLIC int curl_debug_pool = 0;

static struct curl_handle *allocate_handle (void);
static void free_handle (struct curl_handle *);
static int debug_cb (CURL *handle, curl_infotype type,
                     const char *data, size_t size, void *);
static size_t write_cb (char *ptr, size_t size, size_t nmemb, void *opaque);
static size_t read_cb (void *ptr, size_t size, size_t nmemb, void *opaque);
static int get_content_length_accept_range (struct curl_handle *ch);
static bool try_fallback_GET_method (struct curl_handle *ch);
static size_t header_cb (void *ptr, size_t size, size_t nmemb, void *opaque);
static size_t error_cb (char *ptr, size_t size, size_t nmemb, void *opaque);

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

/* Initialize pool structures. */
void
load_pool (void)
{
}

/* Close and free all handles in the pool. */
void
unload_pool (void)
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

/* Allocate and initialize a new libcurl handle. */
static struct curl_handle *
allocate_handle (void)
{
  struct curl_handle *ch;
  CURLcode r;

  ch = calloc (1, sizeof *ch);
  if (ch == NULL) {
    nbdkit_error ("calloc: %m");
    free (ch);
    return NULL;
  }

  ch->c = curl_easy_init ();
  if (ch->c == NULL) {
    nbdkit_error ("curl_easy_init: failed: %m");
    goto err;
  }

  if (curl_debug_verbose) {
    /* NB: Constants must be explicitly long because the parameter is
     * varargs.
     */
    curl_easy_setopt (ch->c, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt (ch->c, CURLOPT_DEBUGFUNCTION, debug_cb);
  }

  curl_easy_setopt (ch->c, CURLOPT_ERRORBUFFER, ch->errbuf);

  r = CURLE_OK;
  if (unix_socket_path) {
#if HAVE_CURLOPT_UNIX_SOCKET_PATH
    r = curl_easy_setopt (ch->c, CURLOPT_UNIX_SOCKET_PATH, unix_socket_path);
#else
    r = CURLE_UNKNOWN_OPTION;
#endif
  }
  if (r != CURLE_OK) {
    display_curl_error (ch, r, "curl_easy_setopt: CURLOPT_UNIX_SOCKET_PATH");
    goto err;
  }

  /* Set the URL. */
  r = curl_easy_setopt (ch->c, CURLOPT_URL, url);
  if (r != CURLE_OK) {
    display_curl_error (ch, r, "curl_easy_setopt: CURLOPT_URL [%s]", url);
    goto err;
  }

  /* Various options we always set.
   *
   * NB: Both here and below constants must be explicitly long because
   * the parameter is varargs.
   *
   * For use of CURLOPT_NOSIGNAL see:
   * https://curl.se/libcurl/c/CURLOPT_NOSIGNAL.html
   */
  curl_easy_setopt (ch->c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt (ch->c, CURLOPT_AUTOREFERER, 1L);
  if (followlocation)
    curl_easy_setopt (ch->c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt (ch->c, CURLOPT_FAILONERROR, 1L);

  /* Options. */
  if (cainfo) {
    if (strlen (cainfo) == 0)
      curl_easy_setopt (ch->c, CURLOPT_CAINFO, NULL);
    else
      curl_easy_setopt (ch->c, CURLOPT_CAINFO, cainfo);
  }
  if (capath)
    curl_easy_setopt (ch->c, CURLOPT_CAPATH, capath);
  if (cookie)
    curl_easy_setopt (ch->c, CURLOPT_COOKIE, cookie);
  if (cookiefile)
    curl_easy_setopt (ch->c, CURLOPT_COOKIEFILE, cookiefile);
  if (cookiejar)
    curl_easy_setopt (ch->c, CURLOPT_COOKIEJAR, cookiejar);
  if (headers)
    curl_easy_setopt (ch->c, CURLOPT_HTTPHEADER, headers);
  if (http_version != CURL_HTTP_VERSION_NONE)
    curl_easy_setopt (ch->c, CURLOPT_HTTP_VERSION, (long) http_version);
  if (ipresolve != CURL_IPRESOLVE_WHATEVER)
    curl_easy_setopt (ch->c, CURLOPT_IPRESOLVE, (long) ipresolve);

  if (password)
    curl_easy_setopt (ch->c, CURLOPT_PASSWORD, password);
#ifndef HAVE_CURLOPT_PROTOCOLS_STR
  if (protocols != CURLPROTO_ALL) {
    curl_easy_setopt (ch->c, CURLOPT_PROTOCOLS, protocols);
    curl_easy_setopt (ch->c, CURLOPT_REDIR_PROTOCOLS, protocols);
  }
#else /* HAVE_CURLOPT_PROTOCOLS_STR (new in 7.85.0) */
  if (protocols) {
    curl_easy_setopt (ch->c, CURLOPT_PROTOCOLS_STR, protocols);
    curl_easy_setopt (ch->c, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
  }
#endif /* HAVE_CURLOPT_PROTOCOLS_STR */
  if (proxy)
    curl_easy_setopt (ch->c, CURLOPT_PROXY, proxy);
  if (proxy_password)
    curl_easy_setopt (ch->c, CURLOPT_PROXYPASSWORD, proxy_password);
  if (proxy_user)
    curl_easy_setopt (ch->c, CURLOPT_PROXYUSERNAME, proxy_user);
  if (!sslverify) {
    curl_easy_setopt (ch->c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt (ch->c, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  if (resolves)
    curl_easy_setopt (ch->c, CURLOPT_RESOLVE, resolves);
  if (ssl_version != CURL_SSLVERSION_DEFAULT)
    curl_easy_setopt (ch->c, CURLOPT_SSLVERSION, (long) ssl_version);
  if (ssl_cipher_list)
    curl_easy_setopt (ch->c, CURLOPT_SSL_CIPHER_LIST, ssl_cipher_list);
  if (tls13_ciphers) {
#if (LIBCURL_VERSION_MAJOR > 7) || \
    (LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 61)
    curl_easy_setopt (ch->c, CURLOPT_TLS13_CIPHERS, tls13_ciphers);
#else
    /* This is not available before curl-7.61 */
    nbdkit_error ("tls13-ciphers is not supported in this build of "
                  "nbdkit-curl-plugin");
    goto err;
#endif
  }
  if (tcp_keepalive)
    curl_easy_setopt (ch->c, CURLOPT_TCP_KEEPALIVE, 1L);
  if (!tcp_nodelay)
    curl_easy_setopt (ch->c, CURLOPT_TCP_NODELAY, 0L);
  if (timeout > 0)
    curl_easy_setopt (ch->c, CURLOPT_TIMEOUT, (long) timeout);
  if (user)
    curl_easy_setopt (ch->c, CURLOPT_USERNAME, user);
  if (user_agent)
    curl_easy_setopt (ch->c, CURLOPT_USERAGENT, user_agent);

  if (get_content_length_accept_range (ch) == -1)
    goto err;

  /* Get set up for reading and writing. */
  curl_easy_setopt (ch->c, CURLOPT_HEADERFUNCTION, NULL);
  curl_easy_setopt (ch->c, CURLOPT_HEADERDATA, NULL);
  curl_easy_setopt (ch->c, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt (ch->c, CURLOPT_WRITEDATA, ch);
  /* These are only used if !readonly but we always register them. */
  curl_easy_setopt (ch->c, CURLOPT_READFUNCTION, read_cb);
  curl_easy_setopt (ch->c, CURLOPT_READDATA, ch);

  return ch;

 err:
  if (ch->c)
    curl_easy_cleanup (ch->c);
  free (ch);
  return NULL;
}

static void
free_handle (struct curl_handle *ch)
{
  curl_easy_cleanup (ch->c);
  if (ch->headers_copy)
    curl_slist_free_all (ch->headers_copy);
  free (ch);
}

/* When using CURLOPT_VERBOSE, this callback is used to redirect
 * messages to nbdkit_debug (instead of stderr).
 */
static int
debug_cb (CURL *handle, curl_infotype type,
          const char *data, size_t size, void *opaque)
{
  size_t origsize = size;
  CLEANUP_FREE char *str;

  /* The data parameter passed is NOT \0-terminated, but also it may
   * have \n or \r\n line endings.  The only sane way to deal with
   * this is to copy the string.  (The data strings may also be
   * multi-line, but we don't deal with that here).
   */
  str = malloc (size + 1);
  if (str == NULL)
    goto out;
  memcpy (str, data, size);
  str[size] = '\0';

  while (size > 0 && (str[size-1] == '\n' || str[size-1] == '\r')) {
    str[size-1] = '\0';
    size--;
  }

  switch (type) {
  case CURLINFO_TEXT:
    nbdkit_debug ("%s", str);
    break;
  case CURLINFO_HEADER_IN:
    nbdkit_debug ("S: %s", str);
    break;
  case CURLINFO_HEADER_OUT:
    nbdkit_debug ("C: %s", str);
    break;
  default:
    /* Assume everything else is binary data that we cannot print. */
    nbdkit_debug ("<data with size=%zu>", origsize);
  }

 out:
  return 0;
}

/* NB: The terminology used by libcurl is confusing!
 *
 * WRITEFUNCTION / write_cb is used when reading from the remote server
 * READFUNCTION / read_cb is used when writing to the remote server.
 *
 * We use the same terminology as libcurl here.
 */

static size_t
write_cb (char *ptr, size_t size, size_t nmemb, void *opaque)
{
  struct curl_handle *ch = opaque;
  size_t orig_realsize = size * nmemb;
  size_t realsize = orig_realsize;

  assert (ch->write_buf);

  /* Don't read more than the requested amount of data, even if the
   * server or libcurl sends more.
   */
  if (realsize > ch->write_count)
    realsize = ch->write_count;

  memcpy (ch->write_buf, ptr, realsize);

  ch->write_count -= realsize;
  ch->write_buf += realsize;

  return orig_realsize;
}

static size_t
read_cb (void *ptr, size_t size, size_t nmemb, void *opaque)
{
  struct curl_handle *ch = opaque;
  size_t realsize = size * nmemb;

  assert (ch->read_buf);
  if (realsize > ch->read_count)
    realsize = ch->read_count;

  memcpy (ptr, ch->read_buf, realsize);

  ch->read_count -= realsize;
  ch->read_buf += realsize;

  return realsize;
}

/* Get the file size and also whether the remote HTTP server
 * supports byte ranges.
 */
static int
get_content_length_accept_range (struct curl_handle *ch)
{
  CURLcode r;
  long code;
#ifdef HAVE_CURLINFO_CONTENT_LENGTH_DOWNLOAD_T
  curl_off_t o;
#else
  double d;
#endif

  /* We must run the scripts if necessary and set headers in the
   * handle.
   */
  if (do_scripts (ch) == -1)
    return -1;

  /* Set this flag in the handle to false.  The callback should set it
   * to true if byte ranges are supported, which we check below.
   */
  ch->accept_range = false;

  /* No Body, not nobody!  This forces a HEAD request. */
  curl_easy_setopt (ch->c, CURLOPT_NOBODY, 1L);
  curl_easy_setopt (ch->c, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt (ch->c, CURLOPT_HEADERDATA, ch);
  r = curl_easy_perform (ch->c);
  if (r != CURLE_OK) {
    display_curl_error (ch, r,
                        "problem doing HEAD request to fetch size of URL [%s]",
                        url);

    /* Get the HTTP status code, if available. */
    r = curl_easy_getinfo (ch->c, CURLINFO_RESPONSE_CODE, &code);
    if (r == CURLE_OK)
      nbdkit_debug ("HTTP status code: %ld", code);
    else
      code = -1;

    /* See comment on try_fallback_GET_method below. */
    if (code != 403 || !try_fallback_GET_method (ch))
      return -1;
  }

  /* Get the content length.
   *
   * Note there is some subtlety here: For web servers using chunked
   * encoding, either the Content-Length header will not be present,
   * or if present it should be ignored.  (For such servers the only
   * way to find out the true length would be to read all of the
   * content, which we don't want to do).
   *
   * Curl itself resolves this for us.  It will ignore the
   * Content-Length header if chunked encoding is used, returning the
   * length as -1 which we check below (see also
   * curl:lib/http.c:Curl_http_size).
   */
#ifdef HAVE_CURLINFO_CONTENT_LENGTH_DOWNLOAD_T
  r = curl_easy_getinfo (ch->c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &o);
  if (r != CURLE_OK) {
    display_curl_error (ch, r,
                        "could not get length of remote file [%s]", url);
    return -1;
  }

  if (o == -1) {
    nbdkit_error ("could not get length of remote file [%s], "
                  "is the URL correct?", url);
    return -1;
  }

  ch->exportsize = o;
#else
  r = curl_easy_getinfo (ch->c, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &d);
  if (r != CURLE_OK) {
    display_curl_error (ch, r,
                        "could not get length of remote file [%s]", url);
    return -1;
  }

  if (d == -1) {
    nbdkit_error ("could not get length of remote file [%s], "
                  "is the URL correct?", url);
    return -1;
  }

  ch->exportsize = d;
#endif
  nbdkit_debug ("content length: %" PRIi64, ch->exportsize);

  /* If this is HTTP, check that byte ranges are supported. */
  if (ascii_strncasecmp (url, "http://", strlen ("http://")) == 0 ||
      ascii_strncasecmp (url, "https://", strlen ("https://")) == 0) {
    if (!ch->accept_range) {
      nbdkit_error ("server does not support 'range' (byte range) requests");
      return -1;
    }

    nbdkit_debug ("accept range supported (for HTTP/HTTPS)");
  }

  return 0;
}

/* S3 servers can return 403 Forbidden for HEAD but still respond
 * to GET, so we give it a second chance in that case.
 * https://github.com/kubevirt/containerized-data-importer/issues/2737
 *
 * This function issues a GET request with a writefunction that always
 * returns an error, thus effectively getting the headers but
 * abandoning the transfer as soon as possible after.
 */
static bool
try_fallback_GET_method (struct curl_handle *ch)
{
  CURLcode r;

  nbdkit_debug ("attempting to fetch headers using GET method");

  curl_easy_setopt (ch->c, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt (ch->c, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt (ch->c, CURLOPT_HEADERDATA, ch);
  curl_easy_setopt (ch->c, CURLOPT_WRITEFUNCTION, error_cb);
  curl_easy_setopt (ch->c, CURLOPT_WRITEDATA, ch);
  r = curl_easy_perform (ch->c);

  /* We expect CURLE_WRITE_ERROR here, but CURLE_OK is possible too
   * (eg if the remote has zero length).  Other errors might happen
   * but we ignore them since it is a fallback path.
   */
  return r == CURLE_OK || r == CURLE_WRITE_ERROR;
}

static size_t
header_cb (void *ptr, size_t size, size_t nmemb, void *opaque)
{
  struct curl_handle *ch = opaque;
  size_t realsize = size * nmemb;
  const char *header = ptr;
  const char *end = header + realsize;
  const char *accept_ranges = "accept-ranges:";
  const char *bytes = "bytes";

  if (realsize >= strlen (accept_ranges) &&
      ascii_strncasecmp (header, accept_ranges, strlen (accept_ranges)) == 0) {
    const char *p = strchr (header, ':') + 1;

    /* Skip whitespace between the header name and value. */
    while (p < end && *p && ascii_isspace (*p))
      p++;

    if (end - p >= strlen (bytes)
        && strncmp (p, bytes, strlen (bytes)) == 0) {
      /* Check that there is nothing but whitespace after the value. */
      p += strlen (bytes);
      while (p < end && *p && ascii_isspace (*p))
        p++;

      if (p == end || !*p)
        ch->accept_range = true;
    }
  }

  return realsize;
}

static size_t
error_cb (char *ptr, size_t size, size_t nmemb, void *opaque)
{
#ifdef CURL_WRITEFUNC_ERROR
  return CURL_WRITEFUNC_ERROR;
#else
  return 0; /* in older curl, any size < requested will also be an error */
#endif
}
