/*
This file is part of GUFI, which is part of MarFS, which is released
under the BSD license.


Copyright (c) 2017, Los Alamos National Security (LANS), LLC
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


From Los Alamos National Security, LLC:
LA-CC-15-039

Copyright (c) 2017, Los Alamos National Security, LLC All rights reserved.
Copyright 2017. Los Alamos National Security, LLC. This software was produced
under U.S. Government contract DE-AC52-06NA25396 for Los Alamos National
Laboratory (LANL), which is operated by Los Alamos National Security, LLC for
the U.S. Department of Energy. The U.S. Government has rights to use,
reproduce, and distribute this software.  NEITHER THE GOVERNMENT NOR LOS
ALAMOS NATIONAL SECURITY, LLC MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR
ASSUMES ANY LIABILITY FOR THE USE OF THIS SOFTWARE.  If software is
modified to produce derivative works, such modified software should be
clearly marked, so as not to confuse it with the version available from
LANL.

THIS SOFTWARE IS PROVIDED BY LOS ALAMOS NATIONAL SECURITY, LLC AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL LOS ALAMOS NATIONAL SECURITY, LLC OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.
*/



#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "QueuePerThreadPool.h"
#include "bf.h"
#include "dbutils.h"
#include "debug.h"
#include "external.h"
#include "utils.h"

struct Unrollup {
    char name[MAXPATH];
    size_t name_len;
    int rolledup; /* set by parent, can be modified by self */
};

static int processdir(QPTPool_t *ctx, const size_t id, void *data, void *args) {
    (void) args;

    struct Unrollup *work = (struct Unrollup *) data;
    int rc = 0;

    DIR *dir = opendir(work->name);
    if (!dir) {
        fprintf(stderr, "Error: Could not open directory \"%s\": %s", work->name, strerror(errno));
        rc = 1;
        goto free_work;
    }

    char dbname[MAXPATH];
    SNPRINTF(dbname, MAXPATH, "%s/" DBNAME, work->name);
    sqlite3 *db = opendb(dbname, SQLITE_OPEN_READWRITE, 0, 0, NULL, NULL);

    if (!db) {
        rc = 1;
    }

    /* get roll up status set by parent */
    int rolledup = work->rolledup;

    /*
     * if parent of this directory was rolled up, all children are rolled up, so skip this check
     * if parent of this directory was not rolled up, this directory might be
     */
    if (db && !rolledup) {
        rc = !!get_rollupscore(db, &rolledup);
    }

    /*
     * always descend
     *     if rolled up, all child also need processing
     *     if not rolled up, children might be
     *
     * descend first to keep working while cleaning up db
     */
    struct dirent *entry = NULL;
    while ((entry = readdir(dir))) {
        const size_t len = strlen(entry->d_name);
        if (len < 3) {
            if ((strncmp(entry->d_name, ".",  2) == 0) ||
                (strncmp(entry->d_name, "..", 3) == 0)) {
                continue;
            }
        }

        char name[MAXPATH];
        size_t name_len = SNPRINTF(name, MAXPATH, "%s/%s", work->name, entry->d_name);

        struct stat st;
        if (lstat(name, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                struct Unrollup *subdir = malloc(sizeof(struct Unrollup));
                memcpy(subdir->name, name, name_len + 1);
                subdir->name_len = name_len;

                /* set child rolledup status so that if this dir */
                /* was rolled up, child can skip roll up check */
                subdir->rolledup = rolledup;

                QPTPool_enqueue(ctx, id, processdir, subdir);
            }
        }
    }

    /* now that work has been pushed onto the queue, clean up this db */
    if (db && rolledup) {
        refstr_t name = {
            .data = work->name,
            .len  = work->name_len,
        };

        char *err = NULL;
        if (sqlite3_exec(db, ROLLUP_CLEANUP, xattrs_rollup_cleanup, &name, &err) != SQLITE_OK) {
            sqlite_print_err_and_free(err, stderr, "Could not remove roll up data from \"%s\": %s\n", work->name, err);
            rc = 1;
        }
        err = NULL;
    }

    closedb(db);
    closedir(dir);

  free_work:
    free(work);

    return rc;
}

static void sub_help(void) {
   printf("GUFI_index        GUFI index to unroll up\n");
   printf("\n");
}

int main(int argc, char *argv[]) {
    struct input in;
    int idx = parse_cmd_line(argc, argv, "hHn:", 1, "GUFI_index ...", &in);
    if (in.helped)
        sub_help();
    if (idx < 0) {
        input_fini(&in);
        return EXIT_FAILURE;
    }

    #if defined(DEBUG) && defined(PER_THREAD_STATS)
    epoch = since_epoch(NULL);

    timestamp_print_init(timestamp_buffers, in.maxthreads + 1, 1024 * 1024, NULL);
    #endif

    QPTPool_t *pool = QPTPool_init(in.maxthreads, NULL);
    if (QPTPool_start(pool) != 0) {
        fprintf(stderr, "Error: Failed to start thread pool\n");
        QPTPool_destroy(pool);
        input_fini(&in);
        return EXIT_FAILURE;
    }

    /* enqueue input paths */
    for(int i = idx; i < argc; i++) {
        size_t len = strlen(argv[i]);
        if (!len) {
            continue;
        }

        len = trim_trailing_slashes(argv[i]);

        struct Unrollup *mywork = malloc(sizeof(struct Unrollup));

        /* copy argv[i] into the work item */
        mywork->name_len = SNFORMAT_S(mywork->name, MAXPATH, 1, argv[i], len);

        /* assume index root was not rolled up */
        mywork->rolledup = 0;

        struct stat st;
        if (lstat(mywork->name, &st) != 0) {
            fprintf(stderr,"Could not stat input-dir '%s'\n", mywork->name);
            free(mywork);
            continue;
        }

        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr,"input-dir '%s' is not a directory\n", mywork->name);
            free(mywork);
            continue;
        }

        /* push the path onto the queue */
        QPTPool_enqueue(pool, i % in.maxthreads, processdir, mywork);
    }

    QPTPool_stop(pool);
    QPTPool_destroy(pool);

    #if defined(DEBUG) && defined(PER_THREAD_STATS)
    timestamp_print_destroy(timestamp_buffers);
    #endif

    input_fini(&in);

    return EXIT_SUCCESS;
}
