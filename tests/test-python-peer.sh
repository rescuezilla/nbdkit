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
set -u

script=$abs_top_srcdir/tests/python-peer.py
test -f "$script"

skip_if_valgrind "because Python code leaks memory"
requires nbdsh --version

# Assume this test only works on Linux.  Other operating systems may
# not support peer credentials or even Unix domain sockets.
requires_linux_kernel_version 2.2

export script
nbdsh -c - <<'EOF'
import os

script = os.environ["script"]

pid = os.getpid()
uid = os.getuid()
gid = os.getgid()

# We pass our PID, UID & GID to the Python plugin.  Since we are
# connecting over a Unix domain socket, the script can check them
# using nbdkit.peer_*.  If they do not match, the script signals this
# by failing the open() call; simply failing to connect is sufficient
# to indicate an error.
h.connect_command(["nbdkit", "-s", "-v", "--exit-with-parent",
                   "python", script,
                   "pid=%d" % pid, "uid=%d" % uid, "gid=%d" % gid])
size = h.get_size()
h.shutdown()
EOF
