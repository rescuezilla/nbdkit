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

# Check that C tests that use requires functions include "requires.h".

source ./functions.sh
set -e

# List of tests in this directory.
tests="$( grep -l '\bmain\b' $srcdir/test-*.c )"

errors=0

for t in $tests; do
    echo checking $t ...
    if grep -sq -- "\brequires[ _]" $t ||
            grep -sq -- "\bskip_because\b" $t; then
        if ! grep -sq -- '"requires.h"' $t; then
            echo "error: $t:" 'test uses requires or skip_because but does not include "requires.h"'
            ((errors++)) ||:
        fi
    else # and the negative:
        if grep -sq -- '"requires.h"' $t; then
            echo "error: $t:" 'test does not use requires or skip_because but includes "requires.h"'
            ((errors++)) ||:
        fi
    fi
done

if [ "$errors" -ge 1 ]; then exit 1; fi
