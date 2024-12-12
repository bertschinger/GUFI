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



/*
iterate through all directories
wait until all subdirectories have been
processed before processing the current one
*/

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "BottomUp.h"
#include "dbutils.h"
#include "utils.h"

/* define so that descend and ascend always have valid functions to call */
static int noop(void *user_struct
                timestamp_sig) {
    #if defined(DEBUG) && defined(PER_THREAD_STATS)
    (void) timestamp_buffers;
    #endif

    (void) user_struct;

    return 0;
}

/* free a struct BottomUp */
void bottomup_destroy(void *p) {
    struct BottomUp *b = (struct BottomUp *) p;

    free(b->name);
    free(b->alt_name);
    free(b);
}

struct UserArgs {
    size_t user_struct_size;
    BU_f descend;
    BU_f ascend;
    int track_non_dirs;
    int generate_alt_name;
    trie_t *skip;

    #if defined(DEBUG) && defined(PER_THREAD_STATS)
    struct OutputBuffers *timestamp_buffers;
    #else
    void *timestamp_buffers;
    #endif
};

/*
 * new_pathname() -
 *   Allocates and initializes a new pathname from the given dirname and basename.
 *
 *     - dirname_len and basename_len should be WITHOUT the null terminator.
 *
 *     - If basename is NULL, then this only uses the dirname and doesn't append a path component
 *       for the basename.
 *
 *   Fills in the path and path length in the given struct BottomUp.
 */
static void new_pathname(struct BottomUp *work, const char *dirname, size_t dirname_len,
                         const char *basename, size_t basename_len) {
    size_t new_len = dirname_len; // does NOT count null terminator

    if (basename) {
        new_len += basename_len + 1; // +1 for path separator
    }

    char *path = calloc(new_len + 1, sizeof(char));  // Additional +1 for null terminator
    if (!path) {
        fprintf(stderr, "%s(): out of memory", __func__);
        /* No point in continuing if we run out of memory: */
        exit(1);
    }

    if (basename) {
        SNFORMAT_S(path, new_len + 1, 3, dirname, dirname_len, "/", (size_t) 1, basename, basename_len);
    } else {
        strncpy(path, dirname, new_len + 1);
    }

    work->name = path;
    work->name_len = new_len;
}

/*
 * new_alt_pathname() -
 *   Allocates and initializes a new alternate pathname, that is, a name that escapes
 *   characters that cannot appear in a sqlite uri.
 *
 *   Parameters [dir|base]name[_len] are the same as in new_pathname().
 *
 *   Fills in alt_name and alt_name_len in the given struct BottomUp.
 *
 *   Returns 0 if conversion succeeded and -1 if conversion failed, because the output
 *   buffer wasn't large enough to hold the new name.
 */
static int new_alt_pathname(struct BottomUp *work, const char *dirname, size_t dirname_len,
                             const char *basename, size_t basename_len) {
    // Alternate name may be longer than the provided dirname and basename together
    char buf[MAXPATH] = { 0 };
    size_t bufsize = sizeof(buf);
    size_t new_len = 0;

    if (basename) {
        // Assume dirname is already sanitized, only need to sanitize basename:
        new_len = SNFORMAT_S(buf, bufsize, 2, dirname, dirname_len, "/", (size_t) 1);

        size_t converted_len = basename_len;
        new_len += sqlite_uri_path(buf + new_len, bufsize - new_len, basename, &converted_len);

        if (converted_len < basename_len) {
            // uh-oh ... didn't convert entire string
            return -1;
        }
    } else {
        // Need to sanitize dirname:
        size_t converted_len = dirname_len;
        new_len = sqlite_uri_path(buf, bufsize, dirname, &converted_len);

        if (converted_len < dirname_len) {
            // uh-oh ... didn't convert entire string
            return -1;
        }
    }

    char *path = calloc(new_len + 1, sizeof(char));
    if (!path) {
        fprintf(stderr, "%s(): out of memory", __func__);
        /* No point in continuing if we run out of memory: */
        exit(1);
    }

    memcpy(path, buf, new_len);

    work->alt_name = path;
    work->alt_name_len = new_len;

    return 0;
}

static int ascend_to_top(QPTPool_t *ctx, const size_t id, void *data, void *args) {
    timestamp_create_start(ascend);

    struct UserArgs *ua = (struct UserArgs *) args;
    struct BottomUp *bu = (struct BottomUp *) data;

    timestamp_create_start(lock_refs);
    pthread_mutex_lock(&bu->refs.mutex);
    timestamp_end_print(ua->timestamp_buffers, id, "lock_refs", lock_refs);

    timestamp_create_start(get_remaining_refs);
    size_t remaining = 0;
    if (bu->refs.remaining) {
        remaining = --bu->refs.remaining;
    }
    timestamp_end_print(ua->timestamp_buffers, id, "get_remaining_refs", get_remaining_refs);

    timestamp_create_start(unlock_refs);
    pthread_mutex_unlock(&bu->refs.mutex);
    timestamp_end_print(ua->timestamp_buffers, id, "unlock_refs", unlock_refs);

    if (remaining) {
        timestamp_end_print(ua->timestamp_buffers, id, "ascend_to_top", ascend);
        return 0;
    }

    /* no subdirectories still need processing, so can process current directory */

    /* keep track of which thread was used to go back up */
    bu->tid.up = id;

    /* call user ascend function */
    timestamp_create_start(run_user_asc_func);
    const int asc_rc = ua->ascend(bu
                                  #if defined(DEBUG) && defined(PER_THREAD_STATS)
                                  , ua->timestamp_buffers
                                  #endif
        );
    timestamp_end_print(ua->timestamp_buffers, id, "run_user_ascend_function", run_user_asc_func);

    /* clean up 'struct BottomUp's here, when they are */
    /* children instead of when they are the parent */
    timestamp_create_start(cleanup);
    sll_destroy(&bu->subdirs, bottomup_destroy);
    sll_destroy(&bu->subnondirs, bottomup_destroy);

    /* mutex is not needed any more */
    pthread_mutex_destroy(&bu->refs.mutex);
    timestamp_end_print(ua->timestamp_buffers, id, "cleanup", cleanup);

    if (bu->parent) {
        /* push parent to decrement their reference counters */
        timestamp_create_start(enqueue_ascend);
        QPTPool_enqueue(ctx, id, ascend_to_top, bu->parent);
        timestamp_end_print(ua->timestamp_buffers, id, "enqueue_ascend", enqueue_ascend);
    }
    else {
        /* reached root */
        timestamp_create_start(free_root);
        bottomup_destroy(bu);
        timestamp_end_print(ua->timestamp_buffers, id, "free_root", free_root);
    }

    timestamp_end_print(ua->timestamp_buffers, id, "ascend_to_top", ascend);
    return asc_rc;
}

/*
 * track() - add the given struct BottomUp src to the given sll_t for later processing.
 *
 * Takes ownership of the name and alt_name from src.
 */
static struct BottomUp *track(struct BottomUp *src,
                              const size_t user_struct_size, sll_t *sll,
                              const size_t level, const int generate_alt_name) {
    struct BottomUp *copy = calloc(user_struct_size, 1);

    copy->name = src->name;
    copy->name_len = src->name_len;

    if (generate_alt_name) {
        copy->alt_name = src->alt_name;
        copy->alt_name_len = src->alt_name_len;
    }

    copy->level = level;

    /* store the subdirectories without enqueuing them */
    sll_push(sll, copy);

    return copy;
}

static int descend_to_bottom(QPTPool_t *ctx, const size_t id, void *data, void *args) {
    timestamp_create_start(descend);

    struct UserArgs *ua = (struct UserArgs *) args;
    struct BottomUp *bu = (struct BottomUp *) data;

    /* keep track of which thread was used to walk downwards */
    bu->tid.down = id;

    timestamp_create_start(open_dir);
    DIR *dir = opendir(bu->name);
    timestamp_end_print(ua->timestamp_buffers, id, "opendir", open_dir);

    if (!dir) {
        fprintf(stderr, "Error: Could not open directory \"%s\": %s\n", bu->name, strerror(errno));
        bottomup_destroy(bu);
        timestamp_end_print(ua->timestamp_buffers, id, "descend_to_bottom", descend);
        return 1;
    }

    timestamp_create_start(init);
    pthread_mutex_init(&bu->refs.mutex, NULL);
    bu->subdir_count = 0;
    bu->subnondir_count = 0;
    sll_init(&bu->subdirs);
    sll_init(&bu->subnondirs);
    timestamp_end_print(ua->timestamp_buffers, id, "init", init);

    timestamp_create_start(read_dir_loop);
    const size_t next_level = bu->level + 1;
    while (1) {
        timestamp_create_start(read_dir);
        struct dirent *entry = readdir(dir);;
        timestamp_end_print(ua->timestamp_buffers, id, "readdir", read_dir);

        if (!entry) {
            break;
        }

        size_t name_len = strlen(entry->d_name);
        if (trie_search(ua->skip, entry->d_name, name_len, NULL) == 1) {
            continue;
        }

        struct BottomUp new_work = { 0 };

        new_pathname(&new_work, bu->name, bu->name_len, entry->d_name, name_len);

        if (ua->generate_alt_name) {
            /* append converted entry name to converted directory */
            new_alt_pathname(&new_work, bu->alt_name, bu->alt_name_len, entry->d_name, name_len);
        }

        timestamp_create_start(lstat_entry);
        const int rc = lstat(new_work.name, &new_work.st);
        timestamp_end_print(ua->timestamp_buffers, id, "lstat", lstat_entry);

        if (rc != 0) {
            fprintf(stderr, "Error: Could not stat \"%s\": %s\n", new_work.name, strerror(errno));
            continue;
        }

        timestamp_create_start(track_entry);
        if (S_ISDIR(new_work.st.st_mode)) {
            track(&new_work,
                  ua->user_struct_size, &bu->subdirs,
                  next_level, ua->generate_alt_name);

            /* count how many subdirectories this directory has */
            bu->subdir_count++;
        }
        else {
            if (ua->track_non_dirs) {
                /* bu takes ownership of new_work's pathname buffers: */
                track(&new_work,
                      ua->user_struct_size, &bu->subnondirs,
                      next_level, ua->generate_alt_name);
            } else {
                /* bu does not take ownership of new_work's pathname buffers, so free them: */
                bottomup_destroy_inner(&new_work);
            }
            bu->subnondir_count++;
        }
        timestamp_end_print(ua->timestamp_buffers, id, "track", track_entry);
    }
    timestamp_end_print(ua->timestamp_buffers, id, "readdir_loop", read_dir_loop);

    timestamp_create_start(close_dir);
    closedir(dir);
    timestamp_end_print(ua->timestamp_buffers, id, "closedir", close_dir);

    /* call user descend function before further descent to ensure */
    /* that the descent function runs before the ascent function */

    /* this will probably be a bottleneck since subdirectories won't */
    /* be queued/processed while the descend function runs */
    timestamp_create_start(run_user_desc_func);
    const int desc_rc = ua->descend(bu
                                    #if defined(DEBUG) && defined(PER_THREAD_STATS)
                                    , ua->timestamp_buffers
                                    #endif
        );
    timestamp_end_print(ua->timestamp_buffers, id, "run_user_descend_function", run_user_desc_func);

    bu->refs.remaining = bu->subdir_count;
    if (desc_rc == 0) {
        /* if there are subdirectories, this directory cannot go back up just yet */
        if (bu->subdir_count) {
            timestamp_create_start(enqueue_subdirs);
            /* have to lock to prevent subdirs from getting popped */
            /* off before all of them have been enqueued */
            pthread_mutex_lock(&bu->refs.mutex);
            sll_loop(&bu->subdirs, node)  {
                struct BottomUp *child = (struct BottomUp *) sll_node_data(node);
                child->parent = bu;
                child->extra_args = bu->extra_args;

                /* keep going down */
                timestamp_create_start(enqueue_subdir);
                QPTPool_enqueue(ctx, id, descend_to_bottom, child);
                timestamp_end_print(ua->timestamp_buffers, id, "enqueue_subdir", enqueue_subdir);
            }
            pthread_mutex_unlock(&bu->refs.mutex);
            timestamp_end_print(ua->timestamp_buffers, id, "enqueue_subdirs", enqueue_subdirs);
        }
        /* start working upwards */
        else {
            timestamp_create_start(enqueue_ascend);
            QPTPool_enqueue(ctx, id, ascend_to_top, bu);
            timestamp_end_print(ua->timestamp_buffers, id, "enqueue_ascend", enqueue_ascend);
        }
    }
    else {
        timestamp_create_start(cleanup_after_error);
        sll_destroy(&bu->subdirs, bottomup_destroy);
        sll_destroy(&bu->subnondirs, bottomup_destroy);
        bu->subdir_count = 0;
        bu->subnondir_count = 0;
        bottomup_destroy(bu);
        timestamp_end_print(ua->timestamp_buffers, id, "cleanup_after_error", cleanup_after_error);
    }

    timestamp_end_print(ua->timestamp_buffers, id, "descend_to_bottom", descend);
    return desc_rc;
}

QPTPool_t *parallel_bottomup_init(const size_t thread_count,
                                  const size_t user_struct_size,
                                  BU_f descend, BU_f ascend,
                                  const int track_non_dirs,
                                  const int generate_alt_name
                                  #if defined(DEBUG) && defined(PER_THREAD_STATS)
                                  , struct OutputBuffers *timestamp_buffers
                                  #endif
    ) {
    if (user_struct_size < sizeof(struct BottomUp)) {
        fprintf(stderr, "Error: Provided user struct size is smaller than a struct BottomUp\n");
        return NULL;
    }

    #if defined(DEBUG) && defined(PER_THREAD_STATS)
    if (timestamp_buffers) {
        if (timestamp_buffers->count <= thread_count) {
            fprintf(stderr, "Error: timestamp_buffers needs at least %zu buffers: Got %zu\n",
                    thread_count + 1, timestamp_buffers->count);
            return NULL;
        }
    }
    #endif

    struct UserArgs *ua = calloc(1, sizeof(*ua));
    ua->user_struct_size = user_struct_size;
    ua->descend = descend?descend:noop;
    ua->ascend = ascend?ascend:noop;
    ua->track_non_dirs = track_non_dirs;
    ua->generate_alt_name = generate_alt_name;

    /* only skip . and .. */
    ua->skip = trie_alloc();
    trie_insert(ua->skip, ".",  1, NULL, NULL);
    trie_insert(ua->skip, "..", 2, NULL, NULL);

    #if defined(DEBUG) && defined(PER_THREAD_STATS)
    ua->timestamp_buffers = timestamp_buffers;
    #endif

    QPTPool_t *pool = QPTPool_init_with_props(thread_count, ua, NULL, NULL, 0, "", 1, 2
                                              #if defined(DEBUG) && defined(PER_THREAD_STATS)
                                              , timestamp_buffers
                                              #endif
        );
    if (QPTPool_start(pool) != 0) {
        fprintf(stderr, "Error: Failed to start thread pool\n");
        QPTPool_destroy(pool);
        trie_free(ua->skip);
        free(ua);
        return NULL;
    }

    return pool;
}

int parallel_bottomup_enqueue(QPTPool_t *pool,
                              const char *path, const size_t len,
                              void *extra_args) {
    if (!pool) {
        return -1;
    }

    size_t thread_count = 0;
    QPTPool_get_nthreads(pool, &thread_count);

    struct UserArgs *ua = NULL;
    QPTPool_get_args(pool, (void **) &ua);

    struct BottomUp *root = (struct BottomUp *) calloc(ua->user_struct_size, 1);
    new_pathname(root, path, len, NULL, 0);

    if (ua->generate_alt_name) {
        int ret = new_alt_pathname(root, root->name, root->name_len, NULL, 0);

        if (ret) {
            fprintf(stderr, "%s could not fit into ALT_NAME buffer\n", root->name);
            bottomup_destroy(root);
            return -1;
        }
    }

    root->parent = NULL;
    root->extra_args = extra_args;
    root->level = 0;

    timestamp_create_start(enqueue_root);
    QPTPool_enqueue(pool, 0, descend_to_bottom, root);
    timestamp_end_print(ua->timestamp_buffers, thread_count, "enqueue_root", enqueue_root);
    return 0;
}

int parallel_bottomup_fini(QPTPool_t *pool) {
    if (!pool) {
        return -1;
    }

    size_t thread_count = 0;
    QPTPool_get_nthreads(pool, &thread_count);

    struct UserArgs *ua = NULL;
    QPTPool_get_args(pool, (void **) &ua);

    timestamp_create_start(qptpool_stop);
    QPTPool_stop(pool);
    timestamp_end_print(ua->timestamp_buffers, thread_count, "wait_for_threads", qptpool_stop);

    const size_t threads_started = QPTPool_threads_started(pool);
    const size_t threads_completed = QPTPool_threads_completed(pool);

    trie_free(ua->skip);
    free(ua);

    #ifdef DEBUG
    fprintf(stderr, "Started %zu threads. Successfully completed %zu threads.\n",
            threads_started, threads_completed);
    #endif

    QPTPool_destroy(pool);

    return -(threads_started != threads_completed);
}

int parallel_bottomup(char **root_names, const size_t root_count,
                      const size_t thread_count,
                      const size_t user_struct_size,
                      BU_f descend, BU_f ascend,
                      const int track_non_dirs,
                      const int generate_alt_name,
                      void *extra_args
                      #if defined(DEBUG) && defined(PER_THREAD_STATS)
                      , struct OutputBuffers *timestamp_buffers
                      #endif
    ) {
    QPTPool_t *pool = parallel_bottomup_init(thread_count, user_struct_size, descend, ascend,
                                             track_non_dirs, generate_alt_name
                                             #if defined(DEBUG) && defined(PER_THREAD_STATS)
                                             , timestamp_buffers
                                             #endif
        );
    if (!pool) {
        return -1;
    }

    struct UserArgs *ua = NULL;
    QPTPool_get_args(pool, (void **) &ua);

    /* enqueue all root directories */
    size_t good_roots = 0;
    timestamp_create_start(enqueue_roots);
    for(size_t i = 0; i < root_count; i++) {
        good_roots += !parallel_bottomup_enqueue(pool, root_names[i], strlen(root_names[i]), extra_args);
    }
    timestamp_end_print(ua->timestamp_buffers, thread_count, "enqueue_roots", enqueue_roots);

    return -(parallel_bottomup_fini(pool) || (root_count != good_roots));
}
