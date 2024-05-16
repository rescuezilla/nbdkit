#!/usr/bin/env bash
# nbdkit
# Copyright Red Hat
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
# * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution.
#
# * Neither the name of Red Hat nor the names of its contributors may be
# used to endorse or promote products derived from this software without
# specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY RED HAT AND CONTRIBUTORS ''AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
# PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL RED HAT OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
# USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
# OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.

source ./functions.sh
set -e
set -x

requires_run
requires_nbdinfo

# Does the nbdkit binary support TLS?
if ! nbdkit --dump-config | grep -sq tls=yes; then
    echo "$0: nbdkit built without TLS support"
    exit 77
fi

# RHEL 7 GnuTLS did not support --tls-verify-peer.
requires nbdkit --tls-verify-peer null --run 'exit 0'

# RHEL 8 libnbd / nbdinfo doesn't support the tls-certificates
# parameter in URIs, so connections always fail.  It's hard to detect
# if libnbd supports this, so just go off version number.  The libnbd
# commit adding this feature was 847e0b9830, added in libnbd 1.9.5.
requires_libnbd_version 1.10

# Did we create the PKI files?
# Probably 'certtool' is missing.
pkidir="$PWD/pki"
if [ ! -f "$pkidir/ca-cert.pem" ]; then
    echo "$0: PKI files were not created by the test harness"
    exit 77
fi

out="tls.out"
rm -f $out
cleanup_fn rm -f $out

export pkidir
nbdkit --tls=require --tls-certificates="$pkidir" --tls-verify-peer \
       -D nbdkit.tls.session=1 \
       example1 \
       --run '
       # Run nbdinfo against the server.
       nbdinfo "$uri"
       ' > $out

cat $out

grep 'is_read_only: true' $out
grep -E 'export-size: 104857600\b' $out
