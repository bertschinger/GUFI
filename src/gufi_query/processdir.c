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



#include <dirent.h>
#include <errno.h>
#include <utime.h>

#if defined(DEBUG) && defined(PER_THREAD_STATS)
#include "OutputBuffers.h"
#endif
#include "bf.h"
#include "compress.h"
#include "dbutils.h"
#include "external.h"
#include "gufi_query/PoolArgs.h"
#include "gufi_query/gqw.h"
#include "gufi_query/process_queries.h"
#include "gufi_query/processdir.h"
#include "gufi_query/timers.h"
#include "utils.h"
#include "xattrs.h"

#define EVRSUMMARY      "e" VRSUMMARY
#define EVRXSUMMARY     "e" VRXSUMMARY
#define EVRPENTRIES     "e" VRPENTRIES
#define EVRXPENTRIES    "e" VRXPENTRIES

static int save_matime(gqw_t *gqw,
                       char *dbpath, const size_t dbpath_size,
                       struct utimbuf *dbtime) {
    const size_t dbpath_len = SNFORMAT_S(dbpath, dbpath_size, 2,
                                         gqw->work.name, gqw->work.name_len,
                                         "/" DBNAME, DBNAME_LEN + 1);
    struct stat st;
    if (lstat(dbpath, &st) != 0) {
        const int err = errno;

        char buf[MAXPATH];
        present_user_path(dbpath, dbpath_len,
                          &gqw->work.root_parent, gqw->work.root_basename_len, &gqw->work.orig_root,
                          buf, sizeof(buf));

        fprintf(stderr, "Could not stat database file \"%s\": %s (%d)\n",
                buf, strerror(err), err);

        return 1;
    }

    dbtime->actime  = st.st_atime;
    dbtime->modtime = st.st_mtime;

    return 0;
}

static int restore_matime(const char *dbpath, struct utimbuf *dbtime) {
    if (utime(dbpath, dbtime) != 0) {
        const int err = errno;
        fprintf(stderr, "Warning: Failed to run utime on database file \"%s\": %s (%d)\n",
                dbpath, strerror(err), err);
    }

    return 0;
}

static int collect_dir_inodes(void *args, int count, char **data, char **columns) {
    (void) count;
    (void) columns;

    sll_t *inodes = (sll_t *) args;

    size_t len = strlen(data[0]);
    char *inode = malloc(len + 1);
    memcpy(inode, data[0], len);
    inode[len] = '\0';

    sll_push(inodes, inode);

    return 0;
}

/* drop view for attaching external dbs to */
static void drop_extdb_views(sqlite3 *db) {
    char *err = NULL;
    if (sqlite3_exec(db,
                     "DROP VIEW " EVRXPENTRIES ";"
                     "DROP VIEW " EVRPENTRIES  ";"
                     "DROP VIEW " EVRXSUMMARY  ";"
                     "DROP VIEW " EVRSUMMARY   ";",
                     NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "Warning: Could not drop views for attaching with external databases: %s\n",
                err);
        sqlite3_free(err);
    }
}

int processdir(QPTPool_t *ctx, const size_t id, void *data, void *args) {
    /* Not checking arguments */

    DIR *dir = NULL;
    sqlite3 *db = NULL;
    struct utimbuf dbtime;

    PoolArgs_t *pa = (PoolArgs_t *) args;
    struct input *in = pa->in;
    ThreadArgs_t *ta = &(pa->ta[id]);

    gqw_t stack;
    gqw_t *gqw = &stack;

    decompress_struct((void **) &gqw, data, sizeof(stack));

    char dbpath[MAXPATH];  /* filesystem path of db.db; only generated if keep_matime is set */
    char dbname[MAXPATH];  /* path of db.db modified so that sqlite3 can open it */
    const size_t dbname_len = SNFORMAT_S(dbname, MAXPATH, 2,
                                         gqw->sqlite3_name, gqw->sqlite3_name_len,
                                         "/" DBNAME, DBNAME_LEN + 1);

    #if defined(DEBUG) && (defined(CUMULATIVE_TIMES) || defined(PER_THREAD_STATS))
    timestamps_t ts;
    timestamps_init(&ts, &pa->start_time);
    #endif

    /* keep opendir near opendb to help speed up sqlite3_open_v2 */
    thread_timestamp_start(ts.tts, opendir_call);
    dir = opendir(gqw->work.name);
    thread_timestamp_end(opendir_call);

    /* if the directory can't be opened, don't bother with anything else */
    if (!dir) {
        goto out_free;
    }

    int rc = 0;
    thread_timestamp_start(ts.tts, lstat_db_call);
    if (in->keep_matime) {
        rc = save_matime(gqw, dbpath, sizeof(dbpath), &dbtime);
    }
    thread_timestamp_end(lstat_db_call);

    if (rc != 0) {
        goto out_free;
    }

    if (gqw->work.level >= in->min_level) {
        #if OPENDB
        thread_timestamp_start(ts.tts, attachdb_call);
        db = attachdb(dbname, ta->outdb, ATTACH_NAME, in->open_flags, 1);
        thread_timestamp_end(attachdb_call);
        increment_query_count(ta);
        #endif

        /* this is needed to add some query functions like path() uidtouser() gidtogroup() */
        #ifdef ADDQUERYFUNCS
        thread_timestamp_start(ts.tts, addqueryfuncs_call);
        if (db) {
            if (addqueryfuncs_with_context(db, &gqw->work) != 0) {
                fprintf(stderr, "Could not add functions to sqlite\n");
            }
        }
        thread_timestamp_end(addqueryfuncs_call);
        #endif
    }

    /* get number of subdirs walked on first call to process_queries */
    size_t subdirs_walked_count = 0;

    /*
     * handle external databases
     *
     * this should be moved into process_queries
     */
    if (db && sll_get_size(&in->external_attach)) {
        char *err = NULL;

        sll_t dir_inodes;
        sll_init(&dir_inodes);

        if (sqlite3_exec(db, "SELECT inode FROM " ATTACH_NAME "." SUMMARY ";",
                         collect_dir_inodes, &dir_inodes, &err) != SQLITE_OK) {
            fprintf(stderr, "Error: Could not pull directory inodes: %s\n", err);
            sqlite3_free(err);
            err = NULL;
            /* ignore errors (still have to descend) */
        }

        /* the only time there are no rows in the summary table is at the index root's parent */
        if (sll_get_size(&dir_inodes) == 0) {
            sll_loop(&in->external_attach, node) {
                eus_t *user = (eus_t *) sll_node_data(node);

                char create_view[MAXSQL];
                SNPRINTF(create_view, sizeof(create_view),
                         "CREATE TEMP VIEW %s AS SELECT * FROM %s;",
                         user->view.data, user->template_table.data
                    );

                if (sqlite3_exec(db, create_view, NULL, NULL, &err) != SQLITE_OK) {
                    fprintf(stderr, "Warning: Could not create view from template at %s: %s\n",
                            gqw->work.name, err);
                    sqlite3_free(err);
                    err = NULL;
                }
            }

            /* create view for attaching external dbs to */
            if (sqlite3_exec(db,
                             "CREATE TEMP VIEW " EVRSUMMARY   " AS SELECT * FROM " VRSUMMARY   ";"
                             "CREATE TEMP VIEW " EVRXSUMMARY  " AS SELECT * FROM " VRXSUMMARY  ";"
                             "CREATE TEMP VIEW " EVRPENTRIES  " AS SELECT * FROM " VRPENTRIES  ";"
                             "CREATE TEMP VIEW " EVRXPENTRIES " AS SELECT * FROM " VRXPENTRIES ";",
                             NULL, NULL, &err) != SQLITE_OK) {
                fprintf(stderr, "Warning: Could not create partition views for attaching with external databases: %s\n", err);
                sqlite3_free(err);
            }

            process_queries(pa, ctx, id, dir, gqw, db, dbname, dbname_len, 1, &subdirs_walked_count
                            #if defined(DEBUG) && (defined(CUMULATIVE_TIMES) || defined(PER_THREAD_STATS))
                            , &ts
                            #endif
                );

            drop_extdb_views(db);

            sll_loop(&in->external_attach, node) {
                eus_t *user = (eus_t *) sll_node_data(node);

                char drop_view[MAXSQL];
                SNPRINTF(drop_view, sizeof(drop_view),
                         "DROP VIEW %s;",
                         user->view.data
                    );

                if (sqlite3_exec(db, drop_view, NULL, NULL, &err) != SQLITE_OK) {
                    fprintf(stderr, "Warning: Could not drop view at %s: %s\n",
                            gqw->work.name, err);
                    sqlite3_free(err);
                    err = NULL;
                }
            }
        }
        else {
            /* don't want to shadow descend function */
            int desc = 1;

            /*
             * for each directory in the summary table, create views
             *
             * each view consists of the empty template table and one
             * external database associated with the current directory
             *
             * there should always be at least one directory (the current one)
             * except at the index root's parent
             */
            sll_loop(&dir_inodes, dir_inode_node) {
                const char *dir_inode = (const char *) sll_node_data(dir_inode_node);
                const size_t dir_inode_len = strlen(dir_inode);

                size_t rec_count = 0;

                /*
                 * for each external database basename provided by the
                 * caller, create a view
                 *
                 * if an external db is not found, only the template is used
                 *
                 * assumes there won't be more than 125 attaches in total
                 * if necessary, change this to attach+query+detach one at a time
                 */
                sll_loop(&in->external_attach, node) {
                    eus_t *user = (eus_t *) sll_node_data(node);

                    char basename_comp[MAXSQL];
                    const size_t basename_comp_len = SNFORMAT_S(basename_comp, sizeof(basename_comp), 7,
                                                                "(pinode == '", (size_t) 12,
                                                                dir_inode, dir_inode_len,
                                                                "')", (size_t) 2,
                                                                " AND ", (size_t) 5,
                                                                "(basename(filename) == '", (size_t) 24,
                                                                user->basename.data, user->basename.len,
                                                                "')", (size_t) 2);
                    const refstr_t basename_comp_ref = {
                        .data = basename_comp,
                        .len  = basename_comp_len,
                    };

                    static const refstr_t SELECT_STAR = {
                        .data = " SELECT * FROM ",
                        .len  = 15,
                    };

                    /*
                     * attach database for current directory (if it
                     * exists) and concatenate with empty template table
                     * into one view
                     */
                    external_concatenate(db,
                                         &EXTERNAL_TYPE_USER_DB,
                                         &basename_comp_ref,
                                         &user->view,
                                         &SELECT_STAR,
                                         &user->table,
                                         &user->template_table,
                                         NULL, NULL,
                                         external_enumerate_attachname, &rec_count
                                         query_count_arg);

                }

                /* create view for attaching external dbs to */
                char extdb_views[MAXSQL];
                SNPRINTF(extdb_views, sizeof(extdb_views),
                         "CREATE TEMP VIEW " EVRSUMMARY   " AS SELECT * FROM " VRSUMMARY   " WHERE  inode == '%s';"
                         "CREATE TEMP VIEW " EVRXSUMMARY  " AS SELECT * FROM " VRXSUMMARY  " WHERE  inode == '%s';"
                         "CREATE TEMP VIEW " EVRPENTRIES  " AS SELECT * FROM " VRPENTRIES  " WHERE pinode == '%s';"
                         "CREATE TEMP VIEW " EVRXPENTRIES " AS SELECT * FROM " VRXPENTRIES " WHERE pinode == '%s';",
                         dir_inode, dir_inode, dir_inode, dir_inode);

                if (sqlite3_exec(db, extdb_views, NULL, NULL, &err) != SQLITE_OK) {
                    fprintf(stderr, "Warning: Could not create partition views for attaching with external databases: %s\n", err);
                    sqlite3_free(err);
                }

                /* run queries */
                process_queries(pa, ctx, id, dir, gqw, db, dbname, dbname_len, desc, &subdirs_walked_count
                                #if defined(DEBUG) && (defined(CUMULATIVE_TIMES) || defined(PER_THREAD_STATS))
                                , &ts
                                #endif
                    );

                drop_extdb_views(db);

                rec_count = 0;
                sll_loop(&in->external_attach, node) {
                    eus_t *user = (eus_t *) sll_node_data(node);

                    /* drop view */
                    char drop_extdb_view[MAXSQL];
                    SNFORMAT_S(drop_extdb_view, sizeof(drop_extdb_view), 3,
                               "DROP VIEW ", (size_t) 10,
                               user->view.data, user->view.len,
                               ";", (size_t) 1);

                    if (sqlite3_exec(db, drop_extdb_view, NULL, NULL, &err) != SQLITE_OK) {
                        fprintf(stderr, "Could not drop view %s: %s\n",
                                user->view.data, err);
                        sqlite3_free(err);
                    }

                    /* detatch associated external database */
                    char attachname[MAXSQL];
                    SNPRINTF(attachname, sizeof(attachname), EXTERNAL_ATTACH_PREFIX "%zu", rec_count++);
                    detachdb(user->basename.data, db, attachname, 0);
                }

                desc = 0;
            }
        }

        sll_destroy(&dir_inodes, free);
    }
    else {
        /*
         * if the database was not opened, still have to descend
         * if no external databases, still have to query
         */
        process_queries(pa, ctx, id, dir, gqw, db, dbname, dbname_len, 1, &subdirs_walked_count
                        #if defined(DEBUG) && (defined(CUMULATIVE_TIMES) || defined(PER_THREAD_STATS))
                        , &ts
                        #endif
            );
    }

    #ifdef OPENDB
    thread_timestamp_start(ts.tts, detachdb_call);
    if (db) {
        detachdb_cached(dbname, db, pa->detach, 1);
        increment_query_count(ta);
    }
    thread_timestamp_end(detachdb_call);
    #endif

    /* restore mtime and atime */
    thread_timestamp_start(ts.tts, utime_call);
    if (db) {
        if (in->keep_matime) {
            restore_matime(dbpath, &dbtime);
        }
    }
    thread_timestamp_end(utime_call);

    thread_timestamp_start(ts.tts, closedir_call);
    closedir(dir);
    thread_timestamp_end(closedir_call);

  out_free:
    ;

    thread_timestamp_start(ts.tts, free_work);
    free(gqw->work.fullpath);
    free_struct(gqw, data, 0);
    thread_timestamp_end(free_work);

    #if defined(DEBUG) && defined(PER_THREAD_STATS)
    struct OutputBuffers *debug_buffers = NULL;
    QPTPool_get_debug_buffers(ctx, &debug_buffers);
    timestamps_print(debug_buffers, id, &ts, dir, db);
    #endif
    #if defined(DEBUG) && defined(CUMULATIVE_TIMES)
    timestamps_sum(&pa->tt, &ts);
    #endif

    #if defined(DEBUG) && (defined(CUMULATIVE_TIMES) || defined(PER_THREAD_STATS))
    timestamps_destroy(&ts);
    #endif

    return 0;
}