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


dir=$(mktemp -d /tmp/nbdkit-test-dir.XXXXXX)
cleanup_fn rm -rf $dir

# Initialization for NBD Device node copied from test-nbd-client.sh 
# FIXME: Replace this with a tool for random access from an NBD URI without associating with a block device,
# similar to nbdcopy. Perhaps create "nbddd" tool?
nbddev=/dev/nbd1
requires_root
requires nbd-client --version
requires_not nbd-client -c $nbddev
requires blockdev --version
requires dd --version
requires_linux_kernel_version 2.2
sock=$(mktemp -u /tmp/nbdkit-test-sock.XXXXXX)
pid=nbd-client.pid
rm -f $sock $pid
cleanup_fn rm -f $sock $pid
cleanup_fn nbd-client -d $nbddev

create_test_input_files() {
    SIZE_IN_BYTE=$1
    FILENAME=$2

    # Use Python to generate binary data (with a fixed seed for reproducibility), redirected to file
    # Note: random data isn't well compressible , so final size may be larger than the original
    python3 -c "import sys,random;random.seed('fixed-seed-for-reproducibility');sys.stdout.buffer.write(bytes(random.getrandbits(8) for _ in range($SIZE_IN_BYTE)))" > $FILENAME
    # Create SHA256 checksum, taking care to read it from stdin so there's no filename associated in the checksum file
    cat "${FILENAME}" | sha256sum --binary > "${FILENAME}.sha256sum"

    # Compress the input
    gzip --keep $FILENAME
}

# Sequentially dump the contents of the NBD URI, and validate all the contents are identical
assert_end_to_end_decompressed_data_identical() {
    FILENAME=$1
    nbdkit --filter=indexed-gzip file \
       file="${FILENAME}.gz" \
       gzip-index-path="${FILENAME}.gzi" \
       --run "nbdcopy \$uri ${FILENAME}.nbdcopy"
       cat "${FILENAME}.nbdcopy" | sha256sum --check ${FILENAME}.sha256sum
}

assert_random_access_decompressed_data_identical() {
    FILENAME=$1
    BLOCK_SIZE=$2
    SKIP_IN_BLOCKS=$3

    original_file_hexdump="${FILENAME}.bs$BLOCK_SIZE.skip.$SKIP_IN_BLOCKS.hexdump"
    # Original file
    dd if=$FILENAME bs=$BLOCK_SIZE count=1 skip=$SKIP_IN_BLOCKS | hexdump -C > $original_file_hexdump

    nbdkit --filter=indexed-gzip file \
       -P $pid -U $sock \
       file="${FILENAME}.gz" \
       gzip-index-path="${FILENAME}.gzi"
    nbd-client -u "nbd+unix://${sock}" $nbddev
    indexed_gzip_file_hexdump="${FILENAME}.bs$BLOCK_SIZE.skip.$SKIP_IN_BLOCKS.indexed-gzip.hexdump"
    dd if=$nbddev bs=$BLOCK_SIZE count=1 skip=$SKIP_IN_BLOCKS | hexdump -C > $indexed_gzip_file_hexdump
    nbd-client -d $nbddev
    kill $pid

    # Compare the two hexdumps
    diff $original_file_hexdump $indexed_gzip_file_hexdump
}

run_test() {
    SIZE_IN_BYTES=$1
    FILENAME=$2

    create_test_input_files "$SIZE_IN_BYTES" "$FILENAME"
    assert_end_to_end_decompressed_data_identical "$FILENAME"
    assert_random_access_decompressed_data_identical "$FILENAME" 1024 0
    assert_random_access_decompressed_data_identical "$FILENAME" 1024 10
    assert_random_access_decompressed_data_identical "$FILENAME" 1024 100
    assert_random_access_decompressed_data_identical "$FILENAME" 1024 1000
    assert_random_access_decompressed_data_identical "$FILENAME" 1024 10000
}

run_test "$((1024 * 1024 * 20))" "$dir/20MiB.img"
run_test "$((1024 * 1024 * 20 - 12345))" "$dir/slightly-less-than-20MiB.img"

# TODO: Testing random access blocks from the NBD URI. 
