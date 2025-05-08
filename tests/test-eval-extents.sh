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
requires_plugin eval
requires_nbdsh_uri
requires nbdsh --base-allocation --version

files="eval-extents.out"
rm -f $files
cleanup_fn rm -f $files

# Trigger an off-by-one bug introduced in v1.11.10 and fixed in v1.43.7
export script='
def f(context, offset, extents, status):
  print(extents)

# First, probe where the server should return 2 extents.
h.block_status(2**32-1, 2, f)

# Next, probe where the server has exactly 2**32-1 bytes in its first extent.
h.block_status(2**32-1, 1, f)

# Now, probe where the first extent has to be truncated.
h.block_status(2**32-1, 0, f)
'
nbdkit eval \
       get_size='echo 5G' \
       pread='dd if=/dev/zero count=$3 iflag=count_bytes' \
       extents='echo 0 4G 1; echo 4G 1G 2' \
       --run 'nbdsh --base-allocation --uri "$uri" -c "$script"' \
       > eval-extents.out
cat eval-extents.out
diff -u - eval-extents.out <<EOF
[4294967294, 1, 1073741824, 2]
[4294967295, 1]
[4294967295, 1]
EOF
