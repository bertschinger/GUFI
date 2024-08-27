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
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "QueuePerThreadPool.h"
#include "bf.h"
#include "debug.h"
#include "dbutils.h"
#include "external.h"
#include "template_db.h"
#include "trace.h"
#include "utils.h"

#if defined(DEBUG) && defined(PER_THREAD_STATS)
#include "OutputBuffers.h"
struct OutputBuffers debug_output_buffers;
#endif

/* global to pool - passed around in "args" argument */
struct PoolArgs {
    struct input in;
    struct template_db db;
    struct template_db xattr;
};

#ifdef DEBUG

#ifdef CUMULATIVE_TIMES
uint64_t total_handle_args      = 0;
uint64_t total_memset_work      = 0;
uint64_t total_dir_linetowork   = 0;
uint64_t total_dupdir           = 0;
uint64_t total_template_to_db   = 0;
uint64_t total_zero_summary     = 0;
uint64_t total_insertdbprep     = 0;
uint64_t total_startdb          = 0;
uint64_t total_read_entries     = 0;
uint64_t total_getline          = 0;
uint64_t total_memset_row       = 0;
uint64_t total_entry_linetowork = 0;
uint64_t total_external_db      = 0;
uint64_t total_sumit            = 0;
uint64_t total_insertdbgo       = 0;
uint64_t total_free             = 0;
uint64_t total_stopdb           = 0;
uint64_t total_insertdbfin      = 0;
uint64_t total_insertsumdb      = 0;
uint64_t total_closedb          = 0;
uint64_t total_row_destroy      = 0;
#endif

#endif

/* process the work under one directory (no recursion) */
static int processdir(QPTPool_t *ctx, const size_t id, void *data, void *args) {
    #ifdef DEBUG
    #ifdef CUMULATIVE_TIMES
    uint64_t thread_handle_args      = 0;
    uint64_t thread_memset_work      = 0;
    uint64_t thread_dir_linetowork   = 0;
    uint64_t thread_dupdir           = 0;
    uint64_t thread_template_to_db   = 0;
    uint64_t thread_zero_summary     = 0;
    uint64_t thread_insertdbprep     = 0;
    uint64_t thread_startdb          = 0;
    uint64_t thread_read_entries     = 0;
    uint64_t thread_getline          = 0;
    uint64_t thread_memset_row       = 0;
    uint64_t thread_entry_linetowork = 0;
    uint64_t thread_external_db      = 0;
    uint64_t thread_sumit            = 0;
    uint64_t thread_insertdbgo       = 0;
    uint64_t thread_free             = 0;
    uint64_t thread_stopdb           = 0;
    uint64_t thread_insertdbfin      = 0;
    uint64_t thread_insertsumdb      = 0;
    uint64_t thread_closedb          = 0;
    uint64_t thread_row_destroy      = 0;
    #endif

    #ifdef PER_THREAD_STATS
    struct OutputBuffers *debug_buffers = NULL;
    QPTPool_get_debug_buffers(ctx, &debug_buffers);
    #endif
    #endif

    timestamp_create_start(handle_args);

    /* Not checking arguments */

    (void) ctx; (void) id;

    struct PoolArgs *pa = (struct PoolArgs *) args;
    struct input *in = &pa->in;
    struct row *w = (struct row *) data;
    const int trace = w->trace;

    timestamp_set_end(handle_args);

    timestamp_create_start(memset_work);
    // TODO: based on following comment, can this be stack allocated?
    struct work *dir = NULL; /* name and name_len are not used */
    struct entry_data ed;
    timestamp_set_end(memset_work);

    /* parse the directory data */
    timestamp_create_start(dir_linetowork);
    linetowork(w->line, w->len, in->delim, &dir, &ed);
    timestamp_set_end(dir_linetowork);

    /* create the directory */
    timestamp_create_start(dupdir);
    const size_t topath_len = in->nameto.len + 1 + w->first_delim;

    /*
     * allocate space for "/db.db" in topath
     *
     * extra buffer is not needed and save on memcpy-ing
     */
    const size_t topath_size = topath_len + 1 + DBNAME_LEN + 1;
    char *topath = malloc(topath_size);
    SNFORMAT_S(topath, topath_size, 4,
               in->nameto.data, in->nameto.len,
               "/", (size_t) 1,
               w->line, w->first_delim,
               "\0" DBNAME, (size_t) 1 + DBNAME_LEN);

    /* have to dupdir here because directories can show up in any order */
    if (dupdir(topath, &ed.statuso)) {
        const int err = errno;
        fprintf(stderr, "Dupdir failure: \"%s\": %s (%d)\n",
                topath, strerror(err), err);
        free(topath);
        row_destroy(&w);
        return 1;
    }
    timestamp_set_end(dupdir);

    timestamp_create_start(template_to_db);

    /* restore "/db.db" (no need to remove afterwards) */
    topath[topath_len] = '/';

    sqlite3 *db = template_to_db(&pa->db, topath, ed.statuso.st_uid, ed.statuso.st_gid);
    timestamp_set_end(template_to_db);

    if (db) {
        timestamp_create_start(zero_summary);
        struct sum summary;
        zeroit(&summary);
        timestamp_set_end(zero_summary);

        sll_t xattr_db_list;
        sll_init(&xattr_db_list);

        /* INSERT statement bindings into db.db */
        timestamp_create_start(insertdbprep);
        sqlite3_stmt *entries_res     = insertdbprep(db, ENTRIES_INSERT);           /* entries */
        sqlite3_stmt *xattrs_res      = insertdbprep(db, XATTRS_PWD_INSERT);        /* xattrs within db.db */
        sqlite3_stmt *xattr_files_res = insertdbprep(db, EXTERNAL_DBS_PWD_INSERT);  /* per-user and per-group db file names */
        timestamp_set_end(insertdbprep);

        timestamp_create_start(startdb);
        startdb(db);
        timestamp_set_end(startdb);

        timestamp_create_start(read_entries);
        size_t row_count = 0;
        char *line = NULL;
        size_t size = 0;
        for(size_t i = 0; i < w->entries; i++) {
            timestamp_create_start(getline);
            const ssize_t len = getline_fd(&line, &size, trace, &w->offset, GETLINE_DEFAULT_SIZE);
            if (len < 1) {
                break;
            }
            timestamp_set_end(getline);

            timestamp_create_start(memset_row);
            struct work *row;
            struct entry_data row_ed;
            timestamp_set_end(memset_row);

            timestamp_create_start(entry_linetowork);
            linetowork(line, len, in->delim, &row, &row_ed);
            timestamp_set_end(entry_linetowork);

            timestamp_create(external_db);
            timestamp_create(sumit);
            timestamp_create(insertdbgo);

            if (row_ed.type == 'e') {
                /* insert right here (instead of bulk inserting) since this is likely to be very rare */
                timestamp_set_start(external_db);
                external_insert(db, EXTERNAL_TYPE_USER_DB_NAME, ed.statuso.st_ino, row->name);
                timestamp_set_end(external_db);
            }
            else {
                /* update summary table */
                timestamp_set_start(sumit);
                sumit(&summary, &row_ed);
                timestamp_set_end(sumit);

                /* don't record pinode */
                row->pinode = 0;

                /* add row to bulk insert */
                timestamp_set_start(insertdbgo);
                insertdbgo(row, &row_ed, entries_res);
                insertdbgo_xattrs(in, &ed.statuso, row, &row_ed,
                                  &xattr_db_list, &pa->xattr,
                                  topath, topath_len,
                                  xattrs_res, xattr_files_res);
                timestamp_set_end(insertdbgo);

                xattrs_cleanup(&row_ed.xattrs);

                row_count++;
                if (row_count > 100000) {
                    timestamp_create_start(stopdb);
                    stopdb(db);
                    timestamp_set_end(stopdb);

                    timestamp_create_start(startdb);
                    startdb(db);
                    timestamp_set_end(startdb);

                    #ifdef DEBUG
                    timestamp_create_start(print_timestamps);
                    timestamp_print    (debug_buffers, id, "startdb",          startdb);
                    timestamp_print    (debug_buffers, id, "stopdb",           stopdb);
                    timestamp_end_print(debug_buffers, id, "print_timestamps", print_timestamps);

                    #ifdef CUMULATIVE_TIMES
                    thread_startdb += timestamp_elapsed(startdb);
                    thread_stopdb  += timestamp_elapsed(stopdb);
                    #endif
                    #endif

                    row_count = 0;
                }
            }

            #ifdef DEBUG
            timestamp_create_start(print_timestamps);
            timestamp_print        (debug_buffers, id, "getline",          getline);
            timestamp_print        (debug_buffers, id, "memset_row",       memset_row);
            timestamp_print        (debug_buffers, id, "entry_linetowork", entry_linetowork);
            if (row_ed.type == 'e') {
                timestamp_print    (debug_buffers, id, "external_db",      external_db);
            }
            else {
                timestamp_print    (debug_buffers, id, "sumit",            sumit);
                timestamp_print    (debug_buffers, id, "insertdbgo",       insertdbgo);
                timestamp_end_print(debug_buffers, id, "print_timestamps", print_timestamps);
            }
            #ifdef CUMULATIVE_TIMES
            thread_getline          += timestamp_elapsed(getline);
            thread_memset_row       += timestamp_elapsed(memset_row);
            thread_entry_linetowork += timestamp_elapsed(entry_linetowork);
            if (row_ed.type == 'e') {
                thread_external_db  += timestamp_elapsed(external_db);
            }
            else {
                thread_sumit        += timestamp_elapsed(sumit);
                thread_insertdbgo   += timestamp_elapsed(insertdbgo);
            }
            #endif

            #endif

            free(row);
        }

        timestamp_create_start(free);
        free(line); /* reuse line and only alloc+free once */
        timestamp_set_end(free);
        timestamp_set_end(read_entries);

        timestamp_create_start(stopdb);
        stopdb(db);
        timestamp_set_end(stopdb);

        /* write out per-user and per-group xattrs */
        sll_destroy(&xattr_db_list, destroy_xattr_db);

        /* write out the current directory's xattrs */
        insertdbgo_xattrs_avail(&ed, xattrs_res);

        /* write out data going into db.db */
        timestamp_create_start(insertdbfin);
        insertdbfin(xattr_files_res); /* per-user and per-group xattr db file names */
        insertdbfin(xattrs_res);
        insertdbfin(entries_res);
        timestamp_set_end(insertdbfin);

        timestamp_create_start(insertsumdb);

        /* find the basename of this path */
        w->line[w->first_delim] = '\x00';
        const size_t basename_start = trailing_match_index(w->line, w->first_delim, "/", 1);

        insertsumdb(db, w->line + basename_start, dir, &ed, &summary);
        xattrs_cleanup(&ed.xattrs);
        timestamp_set_end(insertsumdb);

        timestamp_create_start(closedb);
        closedb(db); /* don't set to nullptr */
        timestamp_set_end(closedb);

        #ifdef DEBUG
        timestamp_create_start(print_timestamps);
        timestamp_print(debug_buffers, id, "zero_summary", zero_summary);
        timestamp_print(debug_buffers, id, "insertdbprep", insertdbprep);
        timestamp_print(debug_buffers, id, "startdb",      startdb);
        timestamp_print(debug_buffers, id, "read_entries", read_entries);
        timestamp_print(debug_buffers, id, "read_entries", read_entries);
        timestamp_print(debug_buffers, id, "free",         free);
        timestamp_print(debug_buffers, id, "stopdb",       stopdb);
        timestamp_print(debug_buffers, id, "insertdbfin",  insertdbfin);
        timestamp_print(debug_buffers, id, "insertsumdb",  insertsumdb);
        timestamp_print(debug_buffers, id, "closedb",      closedb);
        timestamp_set_end(print_timestamps);

        #ifdef CUMULATIVE_TIMES
        thread_zero_summary += timestamp_elapsed(zero_summary);
        thread_insertdbprep += timestamp_elapsed(insertdbprep);
        thread_startdb      += timestamp_elapsed(startdb);
        thread_read_entries += timestamp_elapsed(read_entries);
        thread_free         += timestamp_elapsed(free);
        thread_stopdb       += timestamp_elapsed(stopdb);
        thread_insertdbfin  += timestamp_elapsed(insertdbfin);
        thread_insertsumdb  += timestamp_elapsed(insertsumdb);
        thread_closedb      += timestamp_elapsed(closedb);
        #endif

        timestamp_print(debug_buffers, id, "print_timestamps", print_timestamps);
        #endif
    }

    timestamp_create_start(row_destroy);
    free(topath);
    row_destroy(&w);
    timestamp_set_end(row_destroy);

    #ifdef DEBUG
    timestamp_create_start(print_timestamps);
    timestamp_print(debug_buffers, id, "handle_args",    handle_args);
    timestamp_print(debug_buffers, id, "memset_work",    memset_work);
    timestamp_print(debug_buffers, id, "dir_linetowork", dir_linetowork);
    timestamp_print(debug_buffers, id, "dupdir",         dupdir);
    timestamp_print(debug_buffers, id, "template_to_db", template_to_db);
    timestamp_print(debug_buffers, id, "row_destroy",    row_destroy);
    timestamp_set_end(print_timestamps);
    #ifdef CUMULATIVE_TIMES
    thread_handle_args     += timestamp_elapsed(handle_args);
    thread_memset_work     += timestamp_elapsed(memset_work);
    thread_dir_linetowork  += timestamp_elapsed(dir_linetowork);
    thread_dupdir          += timestamp_elapsed(dupdir);
    thread_template_to_db  += timestamp_elapsed(template_to_db);
    thread_row_destroy     += timestamp_elapsed(row_destroy);

    pthread_mutex_lock(&print_mutex);
    total_handle_args      += thread_handle_args;
    total_memset_work      += thread_memset_work;
    total_dir_linetowork   += thread_dir_linetowork;
    total_dupdir           += thread_dupdir;
    total_template_to_db   += thread_template_to_db;
    total_zero_summary     += thread_zero_summary;
    total_insertdbprep     += thread_insertdbprep;
    total_startdb          += thread_startdb;
    total_read_entries     += thread_read_entries;
    total_getline          += thread_getline;
    total_memset_row       += thread_memset_row;
    total_entry_linetowork += thread_entry_linetowork;
    total_external_db      += thread_external_db;
    total_sumit            += thread_sumit;
    total_insertdbgo       += thread_insertdbgo;
    total_free             += thread_free;
    total_stopdb           += thread_stopdb;
    total_insertdbfin      += thread_insertdbfin;
    total_insertsumdb      += thread_insertsumdb;
    total_closedb          += thread_closedb;
    total_row_destroy      += thread_row_destroy;
    pthread_mutex_unlock(&print_mutex);
    #endif
    timestamp_print(debug_buffers, id, "print_timestamps", print_timestamps);
    #endif

    free(dir);

    return !db;
}

static void sub_help(void) {
   printf("trace_file...     parse one or more trace files to produce the GUFI index\n");
   printf("output_dir        build GUFI index here\n");
   printf("\n");
}


int main(int argc, char *argv[]) {
    /* have to call clock_gettime explicitly to get start time and epoch */
    struct start_end main_func;
    clock_gettime(CLOCK_MONOTONIC, &main_func.start);
    epoch = since_epoch(&main_func.start);

    struct PoolArgs pa;
    int idx = parse_cmd_line(argc, argv, "hHn:d:M:", 2, "trace_file... output_dir", &pa.in);
    if (pa.in.helped)
        sub_help();
    if (idx < 0) {
        input_fini(&pa.in);
        return EXIT_FAILURE;
    }
    else {
        /* parse positional args, following the options */
        INSTALL_STR(&pa.in.nameto, argv[argc - 1]);
    }

    /* open trace files for threads to jump around in */
    /* open the trace files here to not repeatedly open in threads */
    const size_t trace_count = argc - idx - 1;
    int *traces = open_traces(&argv[idx], trace_count);
    if (!traces) {
        fprintf(stderr, "Failed to open trace file for each thread\n");
        input_fini(&pa.in);
        return EXIT_FAILURE;
    }

    int rc = EXIT_SUCCESS;

    init_template_db(&pa.db);
    if (create_dbdb_template(&pa.db) != 0) {
        fprintf(stderr, "Could not create template file\n");
        rc = EXIT_FAILURE;
        goto free_traces;
    }

    struct stat st;
    st.st_mode = S_IRWXU | S_IRWXG | S_IRWXO;
    st.st_uid = geteuid();
    st.st_gid = getegid();

    if (dupdir(pa.in.nameto.data, &st)) {
        fprintf(stderr, "Could not create directory %s\n", pa.in.nameto.data);
        rc = EXIT_FAILURE;
        goto free_traces;
    }

    /*
     * create empty db.db in index parent (this file is placed in
     * "${dst}/db.db"; index is placed in "${dst}/$(basename ${src}))"
     * so that when querying "${dst}", no error is printed
     */
    if (create_empty_dbdb(&pa.db, &pa.in.nameto, geteuid(), getegid()) != 0) {
        rc = EXIT_FAILURE;
        goto free_traces;
    }

    init_template_db(&pa.xattr);
    if (create_xattrs_template(&pa.xattr) != 0) {
        fprintf(stderr, "Could not create xattr template file\n");
        rc = EXIT_FAILURE;
        goto free_db;
    }

    #if defined(DEBUG) && defined(PER_THREAD_STATS)
    OutputBuffers_init(&debug_output_buffers, pa.in.maxthreads, 1073741824ULL, &print_mutex);
    #endif

    const uint64_t queue_depth = pa.in.target_memory_footprint / sizeof(struct work) / pa.in.maxthreads;
    QPTPool_t *pool = QPTPool_init_with_props(pa.in.maxthreads, &pa, NULL, NULL, queue_depth, 1, 2
                                              #if defined(DEBUG) && defined(PER_THREAD_STATS)
                                              , &debug_output_buffers
                                              #endif
        );
    if (QPTPool_start(pool) != 0) {
        fprintf(stderr, "Error: Failed to start thread pool\n");
        QPTPool_destroy(pool);
        rc = -1;
        goto free_xattr;
    }

    fprintf(stdout, "Creating GUFI Index %s with %zu threads\n", pa.in.nameto.data, pa.in.maxthreads);

    /* parse the trace files */
    uint64_t scout_time = 0;
    size_t files = 0;
    size_t dirs = 0;
    size_t empty = 0;
    for(size_t i = 0; i < trace_count; i++) {
        /* copied by fill_scout_args, freed by scout_trace */
        struct ScoutTraceArgs sta = {
            .delim = pa.in.delim,
            .tracename = argv[idx + i],
            .tr.fd = traces[i],
            .processdir = processdir,
            .free = free,
            .mutex = &print_mutex,
            .time = &scout_time,
            .files = &files,
            .dirs = &dirs,
            .empty = &empty,
        };

        split_traces(argv[idx + i], traces[i], pa.in.delim,
                     pa.in.maxthreads,
                     fill_scout_args, &sta,
                     pool, i, scout_trace);
    }

    QPTPool_stop(pool);

    clock_gettime(CLOCK_MONOTONIC, &main_func.end);
    const long double processtime = sec(nsec(&main_func));

    fprintf(stdout, "Scouts took total of %.2Lf seconds (aggregated across %zu threads)\n", sec(scout_time), pa.in.maxthreads);
    fprintf(stdout, "Dirs:                %zu (%zu empty)\n", dirs, empty);
    fprintf(stdout, "Files:               %zu\n", files);
    fprintf(stdout, "Total:               %zu\n", files + dirs);
    fprintf(stdout, "\n");

    /* don't count as part of processtime */
    QPTPool_destroy(pool);

    #if defined(DEBUG) && defined(PER_THREAD_STATS)
    OutputBuffers_flush_to_single(&debug_output_buffers, stderr);
    OutputBuffers_destroy(&debug_output_buffers);
    #endif

    /* set top level permissions */
    chmod(pa.in.nameto.data, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

    #if defined(DEBUG) && defined(CUMULATIVE_TIMES)
    fprintf(stderr, "Handle args:               %.2Lfs\n", sec(total_handle_args));
    fprintf(stderr, "memset(work):              %.2Lfs\n", sec(total_memset_work));
    fprintf(stderr, "Parse directory line:      %.2Lfs\n", sec(total_dir_linetowork));
    fprintf(stderr, "dupdir:                    %.2Lfs\n", sec(total_dupdir));
    fprintf(stderr, "template_to_db:            %.2Lfs\n", sec(total_template_to_db));
    fprintf(stderr, "Zero summary struct:       %.2Lfs\n", sec(total_zero_summary));
    fprintf(stderr, "insertdbprep:              %.2Lfs\n", sec(total_insertdbprep));
    fprintf(stderr, "startdb:                   %.2Lfs\n", sec(total_startdb));
    fprintf(stderr, "Read entries:              %.2Lfs\n", sec(total_read_entries));
    fprintf(stderr, "    getline:               %.2Lfs\n", sec(total_getline));
    fprintf(stderr, "    memset(entry struct):  %.2Lfs\n", sec(total_memset_row));
    fprintf(stderr, "    Parse entry line:      %.2Lfs\n", sec(total_entry_linetowork));
    fprintf(stderr, "    sumit:                 %.2Lfs\n", sec(total_sumit));
    fprintf(stderr, "    insertdbgo:            %.2Lfs\n", sec(total_insertdbgo));
    fprintf(stderr, "free(entry line):          %.2Lfs\n", sec(total_free));
    fprintf(stderr, "stopdb:                    %.2Lfs\n", sec(total_stopdb));
    fprintf(stderr, "insertdbfin:               %.2Lfs\n", sec(total_insertdbfin));
    fprintf(stderr, "insertsumdb:               %.2Lfs\n", sec(total_insertsumdb));
    fprintf(stderr, "closedb:                   %.2Lfs\n", sec(total_closedb));
    fprintf(stderr, "cleanup:                   %.2Lfs\n", sec(total_row_destroy));
    fprintf(stderr, "\n");
    #endif

    fprintf(stdout, "Total Dirs:          %zu\n",    dirs);
    fprintf(stdout, "Total Files:         %zu\n",    files);
    fprintf(stdout, "Time Spent Indexing: %.2Lfs\n", processtime);
    fprintf(stdout, "Dirs/Sec:            %.2Lf\n",  dirs / processtime);
    fprintf(stdout, "Files/Sec:           %.2Lf\n",  files / processtime);

  free_xattr:
    close_template_db(&pa.xattr);
  free_db:
    close_template_db(&pa.db);
  free_traces:
    close_traces(traces, trace_count);

    input_fini(&pa.in);

    return rc;
}
