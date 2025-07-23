#!/usr/bin/env bash
# nbdkit
# Copyright (C) 2025 Shasheen Ediriweera
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
set -u

requires_run

# gztool is a third-party tool used to generate the index file
# https://github.com/circulosmeos/gztool
# TODO: Remove this dependency by making indexed-gzip filter capable of generating the index file itself
requires gztool -h

EXACTLY_20MiB_FILE="20MiB.img"
# Useful to test non-aligned block boundaries
SLIGHTLY_LESS_THAN_20MiB_FILE="slightly-less-than-20MiB.img"

files="$EXACTLY_20MiB_FILE \
       ${EXACTLY_20MiB_FILE}.sha256sum \
       ${EXACTLY_20MiB_FILE}.gz \
       ${EXACTLY_20MiB_FILE}.gzi     \
       $SLIGHTLY_LESS_THAN_20MiB_FILE \
       ${SLIGHTLY_LESS_THAN_20MiB_FILE}.sha256sum \
       ${SLIGHTLY_LESS_THAN_20MiB_FILE}.gz \
       ${SLIGHTLY_LESS_THAN_20MiB_FILE}.gzi"
rm -f $files
#cleanup_fn rm -f $files

create_test_input_files() {
    SIZE_IN_BYTE=$1
    FILENAME=$2

    # Use Python to generate binary data (with a fixed seed for reproducibility), redirected to file
    # Note: random data isn't well compressible , so final size may be larger than the original
    python3 -c "import sys,random;random.seed('fixed-seed-for-reproducibility');sys.stdout.buffer.write(bytes(random.getrandbits(8) for _ in range($SIZE_IN_BYTE)))" > $FILENAME
    # Create SHA256 checksum, taking care to read it from stdin so there's no filename associated in the checksum file
    cat "${FILENAME}" | sha256sum --binary > "${FILENAME}.sha256sum"

    # Compress the input
    gzip $FILENAME
    # Create index file, with a 1MiB span between each index point (rather than default 10MiB)
    # TODO: Use indexed-gzip filter to directly create the index file, once it's capable
    gztool -C "${FILENAME}.gz" -s 1M
}

# Sequentially dump the contents of the NBD URI, and validate all the contents are identical
check_output() {
    FILENAME=$1
    nbdkit --filter=indexed-gzip file \
       file="${FILENAME}.gz" \
       gzip-index-path="${FILENAME}.gzi" \
       --run "nbdcopy \$uri ${FILENAME}.nbdcopy"
       cat "${FILENAME}.nbdcopy" | sha256sum --check ${FILENAME}.sha256sum
}

create_test_input_files "$((1024 * 1024 * 20))" $EXACTLY_20MiB_FILE
create_test_input_files "$((1024 * 1024 * 20 - 12345))" $SLIGHTLY_LESS_THAN_20MiB_FILE

check_output $EXACTLY_20MiB_FILE
check_output $SLIGHTLY_LESS_THAN_20MiB_FILE

# TODO: Testing random access blocks from the NBD URI. 
