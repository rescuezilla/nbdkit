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
#include <string.h>

#include "open_memstream.h"
#include "utils.h"

#include "internal.h"

char *uri;                      /* NBD URI */

char *
make_uri (void)
{
  FILE *fp;
  size_t len = 0;
  char *r = NULL;

  switch (service_mode) {
  case SERVICE_MODE_SOCKET_ACTIVATION:
  case SERVICE_MODE_LISTEN_STDIN:
    /* can't form a URI, uri will be NULL */
    return NULL;
  default: ;
  }

  fp = open_memstream (&r, &len);
  if (fp == NULL) {
    perror ("open_memstream");
    exit (EXIT_FAILURE);
  }

  switch (service_mode) {
  case SERVICE_MODE_UNIXSOCKET:
    fprintf (fp, "nbd%s+unix://", tls == 2 ? "s" : "");
    if (export_name && strcmp (export_name, "") != 0) {
      putc ('/', fp);
      uri_quote (export_name, fp);
    }
    fprintf (fp, "?socket=");
    uri_quote (unixsocket, fp);
    break;
  case SERVICE_MODE_VSOCK:
    /* 1 = VMADDR_CID_LOCAL */
    fprintf (fp, "nbd%s+vsock://1", tls == 2 ? "s" : "");
    if (port) {
      putc (':', fp);
      fputs (port, fp);
    }
    if (export_name && strcmp (export_name, "") != 0) {
      putc ('/', fp);
      uri_quote (export_name, fp);
    }
    break;
  case SERVICE_MODE_TCPIP:
    fprintf (fp, "nbd%s://localhost", tls == 2 ? "s" : "");
    if (port) {
      putc (':', fp);
      fputs (port, fp);
    }
    if (export_name && strcmp (export_name, "") != 0) {
      putc ('/', fp);
      uri_quote (export_name, fp);
    }
    break;

  case SERVICE_MODE_SOCKET_ACTIVATION:
  case SERVICE_MODE_LISTEN_STDIN:
    /* these labels are not-reachable, see above */
    /*FALLTHROUGH*/
  default:
    abort ();
  }

  if (fclose (fp) == EOF) {
    perror ("memstream failed");
    exit (EXIT_FAILURE);
  }

  return r;
}
