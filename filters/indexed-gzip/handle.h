#ifndef HANDLE_H
#define HANDLE_H

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <limits.h>

#include <zlib.h>

#include <nbdkit-filter.h>

#include "cleanup.h"
#include "pread.h"
#include "minmax.h"


/* NBDkit filter handle containing the index */
struct handle {
    struct deflate_index *index;
    uint64_t compressed_size; /* cached size exposed by the NBD filter or plugin the right/upstream of this one */
};

#endif /* HANDLE_H */
