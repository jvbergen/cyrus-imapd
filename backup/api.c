/* api.c -- replication-based backup api
 *
 * Copyright (c) 1994-2015 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <config.h>

#include <assert.h>
#include <errno.h>
#include <syslog.h>
#include <zlib.h>

#include "lib/cyrusdb.h"
#include "lib/cyr_lock.h"
#include "lib/exitcodes.h"
#include "lib/map.h"
#include "lib/sqldb.h"
#include "lib/util.h"
#include "lib/xmalloc.h"
#include "lib/xsha1.h"
#include "lib/xstrlcat.h"
#include "lib/xstrlcpy.h"

#include "imap/dlist.h"
#include "imap/global.h"
#include "imap/imap_err.h"
#include "imap/imapparse.h"

#include "backup/api.h"
#include "backup/gzuncat.h"
#include "backup/sqlconsts.h"

#define BACKUP_INTERNAL_SOURCE /* this file is part of the backup API */
#include "backup/internal.h"

static int _column_int(sqlite3_stmt *stmt, int column);
static sqlite3_int64 _column_int64(sqlite3_stmt *stmt, int column);
static char * _column_text(sqlite3_stmt *stmt, int column);
static const char *_sha1_file(int fd, const char *fname, size_t limit,
                              char buf[2 * SHA1_DIGEST_LENGTH + 1]);

/*
 * use cases:
 *  - backupd needs to be able to append to data stream and update index (exclusive)
 *  - backupd maybe needs to create a new backup from scratch (exclusive)
 *  - reindex needs to gzuc data stream and rewrite index (exclusive)
 *  - compress needs to rewrite data stream and index (exclusive)
 *  - restore needs to read data stream and index (shared)
 *
 * with only one shared case, might as well always lock exclusively...
 */
enum backup_open_mode {
    BACKUP_OPEN_NORMAL = 0,
    BACKUP_OPEN_REINDEX,
};

static struct backup *_open_internal(const char *data_fname,
                                     const char *index_fname,
                                     enum backup_open_mode mode)
{
    struct backup *backup = xzmalloc(sizeof *backup);
    if (!backup) return NULL;

    backup->fd = -1;

    backup->data_fname = xstrdup(data_fname);
    backup->index_fname = xstrdup(index_fname);

    backup->fd = open(backup->data_fname,
                      O_RDWR | O_CREAT | O_APPEND,
                      S_IRUSR | S_IWUSR);
    if (backup->fd < 0) {
        syslog(LOG_ERR, "IOERROR: open %s: %m", backup->data_fname);
        goto error;
    }

    int r = lock_setlock(backup->fd, /*excl*/ 1, /*nb*/ 0, backup->data_fname);
    if (r) {
        syslog(LOG_ERR, "IOERROR: lock_setlock: %s: %m", backup->data_fname);
        goto error;
    }

    if (mode == BACKUP_OPEN_REINDEX) {
        // when reindexing, we want to move the old index out of the way
        // and create a new, empty one -- while holding the lock
        char *oldindex_fname = strconcat(backup->index_fname, ".old", NULL);

        r = rename(backup->index_fname, oldindex_fname);
        if (r && errno != ENOENT) {
            syslog(LOG_ERR, "IOERROR: rename %s %s: %m", backup->index_fname, oldindex_fname);
            free(oldindex_fname);
            goto error;
        }

        backup->oldindex_fname = oldindex_fname;
    }
    else {
        // if there's data in the data file but the index file is empty
        // or doesn't exist, insist on a reindex before opening
        struct stat data_statbuf;
        r = fstat(backup->fd, &data_statbuf);
        if (r) {
            syslog(LOG_ERR, "IOERROR: fstat %s: %m", backup->data_fname);
            goto error;
        }
        if (data_statbuf.st_size > 0) {
            struct stat index_statbuf;
            r = stat(backup->index_fname, &index_statbuf);
            if (r && errno != ENOENT) {
                syslog(LOG_ERR, "IOERROR: stat %s: %m", backup->index_fname);
                goto error;
            }

            if (errno == ENOENT || index_statbuf.st_size == 0) {
                syslog(LOG_ERR, "reindex needed: %s", backup->index_fname);
                goto error;
            }
        }
    }

    backup->db = sqldb_open(backup->index_fname, backup_index_initsql,
                            backup_index_version, backup_index_upgrade);
    if (!backup->db) goto error;

    // FIXME detect when last append didn't end correctly (no length/data_sha1)
    // and insist on reindex (can this happen with txns?)

    return backup;

error:
    backup_close(&backup);
    return NULL;
}

struct backup_meta {
    int id;
    time_t timestamp;
    off_t offset;
    size_t length;
    char *file_sha1;
    char *data_sha1;
};

static int _validate_cb(sqlite3_stmt *stmt, void *rock)
{
    struct backup_meta *backup_meta = (struct backup_meta *) rock;

    int column = 0;
    backup_meta->id = _column_int(stmt, column++);
    backup_meta->timestamp = _column_int64(stmt, column++);
    backup_meta->offset = _column_int64(stmt, column++);
    backup_meta->length = _column_int64(stmt, column++);
    backup_meta->file_sha1 = _column_text(stmt, column++);
    backup_meta->data_sha1 = _column_text(stmt, column++);

    return 0;
}

static int _validate_checksums(struct backup *backup)
{
    struct backup_meta backup_meta = {0};
    struct gzuncat *gzuc = NULL;

    int r = sqldb_exec(backup->db, backup_index_backup_select_latest_sql,
                       NULL, _validate_cb, &backup_meta);
    if (r) goto done;
    if (!backup_meta.id) {
        fprintf(stderr, "%s: %s file checksum mismatch: not in index\n",
                __func__, backup->data_fname);
        r = -1;
        goto done;
    }

    /* validate file-prior-to-this-backup checksum */
    char file_sha1[2 * SHA1_DIGEST_LENGTH + 1];
    _sha1_file(backup->fd, backup->data_fname, backup_meta.offset, file_sha1);
    r = strncmp(backup_meta.file_sha1, file_sha1, sizeof(file_sha1));
    if (r) {
        fprintf(stderr, "%s: %s file checksum mismatch: %s on disk, %s in index\n",
                __func__, backup->data_fname, file_sha1, backup_meta.file_sha1);
        goto done;
    }

    /* validate data-within-this-backup checksum */
    gzuc = gzuc_open(backup->fd);
    if (!gzuc) {
        r = -1;
        goto done;
    }

    char buf[8192]; /* FIXME whatever */
    size_t len = 0;
    SHA_CTX sha_ctx;
    SHA1_Init(&sha_ctx);
    gzuc_member_start_from(gzuc, backup_meta.offset);
    while (!gzuc_member_eof(gzuc)) {
        ssize_t n = gzuc_read(gzuc, buf, sizeof(buf));
        if (n >= 0) {
            SHA1_Update(&sha_ctx, buf, n);
            len += n;
        }
    }
    if (len != backup_meta.length) {
        r = -1;
        goto done;
    }
    unsigned char sha1_raw[SHA1_DIGEST_LENGTH];
    char data_sha1[2 * SHA1_DIGEST_LENGTH + 1];
    SHA1_Final(sha1_raw, &sha_ctx);
    r = bin_to_hex(sha1_raw, SHA1_DIGEST_LENGTH, data_sha1, BH_LOWER);
    assert(r == 2 * SHA1_DIGEST_LENGTH);
    r = strncmp(backup_meta.data_sha1, data_sha1, sizeof(data_sha1));
    if (r) {
        fprintf(stderr, "%s: %s data checksum mismatch: %s on disk, %s in index\n",
                __func__, backup->data_fname, data_sha1, backup_meta.data_sha1);
        goto done;
    }

done:
    if (gzuc) gzuc_close(&gzuc);
    free(backup_meta.file_sha1);
    free(backup_meta.data_sha1);
    fprintf(stderr, "%s: checksum %s!\n", __func__, r ? "failed" : "passed");
    return r;
}

EXPORTED struct backup *backup_open(const mbname_t *mbname)
{
    struct buf data_fname = BUF_INITIALIZER;
    struct buf index_fname = BUF_INITIALIZER;
    struct backup *backup = NULL;

    int r = backup_get_paths(mbname, &data_fname, &index_fname);
    if (r) goto done;

    backup = _open_internal(buf_cstring(&data_fname),
                            buf_cstring(&index_fname),
                            BACKUP_OPEN_NORMAL);
    r = _validate_checksums(backup);
    if (r) backup_close(&backup);

done:
    buf_free(&data_fname);
    buf_free(&index_fname);

    return backup;
}

/* Uses mkstemp() to create a new, unique, backup path for the given user.
 *
 * On success, the file is not unlinked, presuming that it will shortly be
 * used for storing backup data.  This also ensures its uniqueness remains:
 * this function won't generate the same value again as long as the previous
 * file is intact, so there's no user-rename race.
 *
 * If out_fd is non-NULL, on successful return it will contain an open, locked
 * file descriptor for the new file.  In this case the caller must unlock
 * and close the fd.
 *
 * On error, returns NULL and logs to syslog, without touching out_fd.
 */
static const char *backup_make_path(const mbname_t *mbname, int *out_fd)
{
    char pathresult[PATH_MAX];

    const char *userid = mbname_userid(mbname);
    const char *backup_data_path = config_getstring(IMAPOPT_BACKUP_DATA_PATH);
    const char *ret = NULL;

    if (!backup_data_path) {
        syslog(LOG_ERR,
               "unable to make backup path for %s: "
               "no backup_data_path defined in imapd.conf",
               userid);
        return NULL;
    }

    char hash_buf[2];
    char *template = strconcat(backup_data_path,
                               "/", dir_hash_b(userid, 1, hash_buf),
                               "/", userid, "_XXXXXX",
                               NULL);

    /* make sure the destination directory exists */
    cyrus_mkdir(template, 0755);

    int fd = mkstemp(template);
    if (fd >= 0) {
        if (strlcpy(pathresult, template, sizeof(pathresult)) < sizeof(pathresult)) {
            ret = pathresult;
        }
        else {
            syslog(LOG_ERR,
                   "unable to make backup path for %s: path too long",
                   userid);
            unlink(template);
        }

        /* if we're holding the fd open, lock it */
        if (out_fd) {
            int r = lock_setlock(fd, /*excl*/ 1, /*nb*/ 0, template);
            if (r) {
                syslog(LOG_ERR,
                       "unable to obtain exclusive lock on just-created file %s: %m",
                       template);
                /* don't unlink it, we don't know what's in it anymore, just return failure */
                ret = NULL;
            }
            else {
                *out_fd = fd;
            }
        }
        else {
            close(fd);
        }
    }
    else {
        syslog(LOG_ERR, "unable to make backup path for %s: %m", userid);
    }

    free(template);
    return ret;
}

EXPORTED int backup_get_paths(const mbname_t *mbname,
                              struct buf *data_fname, struct buf *index_fname)
{
    char *backups_db_fname = xstrdup(config_getstring(IMAPOPT_BACKUPS_DB_PATH));
    if (!backups_db_fname)
        backups_db_fname = strconcat(config_dir, "/backups.db", NULL);

    struct db *backups_db = NULL;
    struct txn *tid = NULL;

    int r = cyrusdb_open(config_backups_db, backups_db_fname, CYRUSDB_CREATE,
                         &backups_db);
    if (r) goto done;

    const char *userid = mbname_userid(mbname);
    const char *backup_path = NULL;
    size_t path_len = 0;

    r = cyrusdb_fetch(backups_db,
                      userid, strlen(userid),
                      &backup_path, &path_len,
                      &tid);

    if (r == CYRUSDB_NOTFOUND) {
        backup_path = backup_make_path(mbname, NULL);
        if (!backup_path) {
            r = IMAP_INTERNAL; /* FIXME ?? */
            goto done;
        }
        path_len = strlen(backup_path);

        r = cyrusdb_create(backups_db,
                           userid, strlen(userid),
                           backup_path, path_len,
                           &tid);

        /* if we didn't store it in the database successfully, trash the file,
         * it won't be used */
        if (r) unlink(backup_path);
    }

    if (r) goto done;

    if (path_len == 0) {
        syslog(LOG_DEBUG,
               "unexpectedly got zero length backup path for user %s",
               userid);
        r = IMAP_INTERNAL; /* FIXME ?? */
        goto done;
    }

    buf_setmap(data_fname, backup_path, path_len);

    buf_setmap(index_fname, backup_path, path_len);
    buf_appendcstr(index_fname, ".index");

done:
    if (tid)
        cyrusdb_commit(backups_db, tid);
    if (backups_db)
        cyrusdb_close(backups_db);
    free(backups_db_fname);
    return r;
}

/*
 * If index_fname is NULL, it will be automatically derived from data_fname
 */
EXPORTED struct backup *backup_open_paths(const char *data_fname,
                                          const char *index_fname)
{
    if (index_fname)
        return _open_internal(data_fname, index_fname, BACKUP_OPEN_NORMAL);

    char *tmp = strconcat(data_fname, ".index", NULL);
    struct backup *backup = _open_internal(data_fname, tmp, BACKUP_OPEN_NORMAL);
    free(tmp);

    if (backup) {
        int r = _validate_checksums(backup);
        if (r) backup_close(&backup);
    }

    return backup;
}

EXPORTED int backup_close(struct backup **backupp)
{
    struct backup *backup = *backupp;
    *backupp = NULL;

    int r1 = 0, r2 = 0;

    if (backup->append_state)
        r1 = backup_append_end(backup);

    if (backup->db) r2 = sqldb_close(&backup->db);

    if (r2 && backup->oldindex_fname) {
        rename(backup->oldindex_fname, backup->index_fname);
        free(backup->oldindex_fname);
    }

    if (backup->fd >= 0) {
        lock_unlock(backup->fd, backup->data_fname);
        close(backup->fd);
    }

    if (backup->index_fname) free(backup->index_fname);
    if (backup->data_fname) free(backup->data_fname);

    free(backup);
    return r1 ? r1 : r2;
}

EXPORTED const char *backup_get_data_fname(const struct backup *backup)
{
    return backup->data_fname;
}

EXPORTED const char *backup_get_index_fname(const struct backup *backup)
{
    return backup->index_fname;
}

static int _column_int(sqlite3_stmt *stmt, int column)
{
    assert(sqlite3_column_type(stmt, column) == SQLITE_INTEGER);
    return sqlite3_column_int(stmt, column);
}

static sqlite3_int64 _column_int64(sqlite3_stmt *stmt, int column)
{
    assert(sqlite3_column_type(stmt, column) == SQLITE_INTEGER);
    return sqlite3_column_int64(stmt, column);
}

static char * _column_text(sqlite3_stmt *stmt, int column)
{
    assert(sqlite3_column_type(stmt, column) == SQLITE_TEXT);
    return xstrdup((const char *) sqlite3_column_text(stmt, column));
}

static int _get_mailbox_id_cb(sqlite3_stmt *stmt, void *rock) {
    int *idp = (int *) rock;

    *idp = _column_int(stmt, 0);

    return 0;
}

EXPORTED int backup_get_mailbox_id(struct backup *backup, const char *uniqueid)
{
    struct sqldb_bindval bval[] = {
        { ":uniqueid",  SQLITE_TEXT,    { .s = uniqueid } },
        { NULL,         SQLITE_NULL,    { .s = NULL } },
    };

    int id = -1;

    int r = sqldb_exec(backup->db, backup_index_mailbox_select_uniqueid_sql,
                       bval, _get_mailbox_id_cb, &id);
    if (r)
        fprintf(stderr, "%s: something went wrong: %i %s\n", __func__, r, uniqueid);

    return id;
}

struct _mailbox_row_rock {
    sqldb_t *db;
    backup_mailbox_foreach_cb proc;
    void *rock;
    struct backup_mailbox **save;
    int want_records;
};

static int _mailbox_message_row_cb(sqlite3_stmt *stmt, void *rock)
{
    struct dlist *parent = (struct dlist *) rock;

    struct dlist *record = dlist_newkvlist(parent, NULL);
    const char *flag_str = NULL;
    const char *annot_str = NULL;
    int r = 0;

    int column = 4;  // skip unused columns
    dlist_setnum32(record, "UID", _column_int(stmt, column++));
    dlist_setnum64(record, "MODSEQ", _column_int64(stmt, column++));
    dlist_setdate(record, "LAST_UPDATE", _column_int64(stmt, column++));
    flag_str = _column_text(stmt, column++);
    dlist_setdate(record, "INTERNALDATE", _column_int64(stmt, column++));
    dlist_setatom(record, "GUID", _column_text(stmt, column++)); // FIXME dlist_setguid?
    dlist_setnum32(record, "SIZE", _column_int(stmt, column++));
    annot_str = _column_text(stmt, column++);

    if (flag_str && flag_str[0]) {
        struct dlist *flags = NULL;
        r = dlist_parsemap(&flags, 0, flag_str, strlen(flag_str));
        if (r) return r; // FIXME handle this sanely
        if (flags) {
            flags->name = xstrdup("FLAGS");
            dlist_stitch(record, flags);
        }
    }

    if (annot_str && annot_str[0]) {
        struct dlist *annots = NULL;
        r = dlist_parsemap(&annots, 0, annot_str, strlen(annot_str));
        if (r) return r; // FIXME handle this sanely
        if (annots) {
            annots->name = xstrdup("ANNOTATIONS");
            dlist_stitch(record, annots);
        }
    }

    return r;
}

static int _mailbox_row_cb(sqlite3_stmt *stmt, void *rock)
{
    struct _mailbox_row_rock *mbrock = (struct _mailbox_row_rock *) rock;

    struct backup_mailbox *mailbox = xzmalloc(sizeof *mailbox);
    struct dlist *dl = dlist_newkvlist(NULL, "MAILBOX");
    int r = 0;
    const char *annot_str = NULL;

    int column = 0;
    mailbox->id = _column_int(stmt, column++);
    mailbox->last_backup_id = _column_int(stmt, column++);
    dlist_setatom(dl, "UNIQUEID", _column_text(stmt, column++));
    dlist_setatom(dl, "MBOXNAME", _column_text(stmt, column++));
    dlist_setatom(dl, "MBOXTYPE", _column_text(stmt, column++));
    dlist_setnum32(dl, "LAST_UID", _column_int(stmt, column++));
    dlist_setnum64(dl, "HIGHESTMODSEQ", _column_int64(stmt, column++));
    dlist_setnum32(dl, "RECENTUID", _column_int(stmt, column++));
    dlist_setdate(dl, "RECENTTIME", _column_int64(stmt, column++));
    dlist_setdate(dl, "LAST_APPENDDATE", _column_int64(stmt, column++));
    dlist_setdate(dl, "POP3_LAST_LOGIN", _column_int64(stmt, column++));
    dlist_setdate(dl, "POP3_SHOW_AFTER", _column_int64(stmt, column++));
    dlist_setnum32(dl, "UIDVALIDITY", _column_int(stmt, column++));
    dlist_setatom(dl, "PARTITION", _column_text(stmt, column++));
    dlist_setatom(dl, "ACL", _column_text(stmt, column++));
    dlist_setatom(dl, "OPTIONS", _column_text(stmt, column++));
    dlist_setnum32(dl, "SYNC_CRC", _column_int(stmt, column++));
    dlist_setnum32(dl, "SYNC_CRC_ANNOT", _column_int(stmt, column++));
    dlist_setatom(dl, "QUOTAROOT", _column_text(stmt, column++));
    dlist_setnum64(dl, "XCONVMODSEQ", _column_int64(stmt, column++));
    annot_str = _column_text(stmt, column++);
    mailbox->deleted = _column_int(stmt, column++);

    if (annot_str && annot_str[0]) {
        struct dlist *annots = NULL;
        r = dlist_parsemap(&annots, 0, annot_str, strlen(annot_str));
        if (r) return r; // FIXME handle this sanely
        if (annots) {
            annots->name = xstrdup("ANNOTATIONS");
            dlist_stitch(dl, annots);
        }
    }

    if (mbrock->want_records) {
        struct dlist *records = dlist_newlist(NULL, "RECORD");

        struct sqldb_bindval bval[] = {
            { ":mailbox_id",    SQLITE_INTEGER, { .i = mailbox->id } },
            { NULL,             SQLITE_NULL,    { .s = NULL } },
        };

        r = sqldb_exec(mbrock->db,
                       backup_index_mailbox_message_select_mailbox_sql,
                       bval,
                       _mailbox_message_row_cb, records);

        if (!r)
            dlist_stitch(dl, records);

        // FIXME sensible error handling
    }

    mailbox->dlist = dl;

    if (mbrock->proc)
        r = mbrock->proc(mailbox, mbrock->rock);

    if (mbrock->save)
        *mbrock->save = mailbox;
    else
    backup_mailbox_free(&mailbox);

    return r;
}

EXPORTED int backup_mailbox_foreach(struct backup *backup,
                                    int want_records,
                                    backup_mailbox_foreach_cb cb,
                                    void *rock)
{
    struct _mailbox_row_rock mbrock = { backup->db, cb, rock, NULL, want_records};

    int r = sqldb_exec(backup->db, backup_index_mailbox_select_all_sql, NULL,
                       _mailbox_row_cb, &mbrock);

    return r;
}

EXPORTED struct backup_mailbox *backup_get_mailbox_by_name(struct backup *backup,
                                                  const mbname_t *mbname,
                                                  int want_records)
{
    struct backup_mailbox *mailbox = NULL;

    struct _mailbox_row_rock mbrock = { backup->db, NULL, NULL, &mailbox,
                                        want_records };

    struct sqldb_bindval bval[] = {
        { ":mboxname",  SQLITE_TEXT,    { .s = mbname_intname(mbname) } },
        { NULL,         SQLITE_NULL,    { .s = NULL } },
    };

    int r = sqldb_exec(backup->db, backup_index_mailbox_select_mboxname_sql,
                       bval, _mailbox_row_cb, &mbrock);

    if (r) {
        if (mailbox) backup_mailbox_free(&mailbox);
        return NULL;
    }

    return mailbox;
}

EXPORTED void backup_mailbox_free(struct backup_mailbox **mailboxp)
{
    struct backup_mailbox *mailbox = *mailboxp;
    *mailboxp = NULL;

    if (mailbox->dlist) dlist_free(&mailbox->dlist);

    free(mailbox);
}

static int _get_message_id_cb(sqlite3_stmt *stmt, void *rock) {
    int *idp = (int *) rock;

    *idp = _column_int(stmt, 0);

    return 0;
}

EXPORTED int backup_get_message_id(struct backup *backup, const char *guid)
{
    struct sqldb_bindval bval[] = {
        { ":guid",  SQLITE_TEXT,    { .s = guid } },
        { NULL,     SQLITE_NULL,    { .s = NULL } },
    };

    // FIXME distinguish between error and not found
    int id = -1;

    int r = sqldb_exec(backup->db, backup_index_message_select_guid_sql, bval,
                       _get_message_id_cb, &id);
    if (r)
        fprintf(stderr, "%s: something went wrong: %i %s\n", __func__, r, guid);

    return id;
}

static int _get_message_cb(sqlite3_stmt *stmt, void *rock) {
    struct backup_message *message = (struct backup_message *) rock;
    int column = 0;

    message->id = _column_int(stmt, column++);
    char *guid_str = _column_text(stmt, column++);
    message->partition = _column_text(stmt, column++);
    message->backup_id = _column_int(stmt, column++);
    message->offset = _column_int64(stmt, column++);
    message->length = _column_int64(stmt, column++);

    struct message_guid *guid = xzmalloc(sizeof *guid);
    if (!message_guid_decode(guid, guid_str)) goto error;
    message->guid = guid;
    free(guid_str);

    return 0;

error:
    if (guid && !message->guid) free(guid);
    if (guid_str) free(guid_str);
    return -1;
}

EXPORTED struct backup_message *backup_get_message(struct backup *backup,
                                                   const struct message_guid *guid)
{
    struct sqldb_bindval bval[] = {
        { ":guid",  SQLITE_TEXT,    { .s = message_guid_encode(guid) } },
        { NULL,     SQLITE_NULL,    { .s = NULL } },
    };

    struct backup_message *bm = xzmalloc(sizeof *bm);

    int r = sqldb_exec(backup->db, backup_index_message_select_guid_sql, bval,
                       _get_message_cb, bm);
    if (r) goto error;

    return bm;

error:
    fprintf(stderr, "%s: something went wrong: %i %s\n", __func__, r, message_guid_encode(guid));
    if (bm) backup_message_free(&bm);
    return NULL;
}

EXPORTED void backup_message_free(struct backup_message **messagep)
{
    struct backup_message *message = *messagep;
    *messagep = NULL;

    if (message->guid) free(message->guid);
    if (message->partition) free(message->partition);

    free(message);
}

/* limit is how much of the file to calculate the sha1 of (in bytes),
 * or SHA1_LIMIT_WHOLE_FILE for the whole file */
#define SHA1_LIMIT_WHOLE_FILE ((size_t) -1)
static const char *_sha1_file(int fd, const char *fname, size_t limit,
                              char buf[2 * SHA1_DIGEST_LENGTH + 1])
{
    const char *map = NULL;
    size_t len = 0, calc_len;
    unsigned char sha1_raw[SHA1_DIGEST_LENGTH];
    int r;

    map_refresh(fd, /*onceonly*/ 1, &map, &len, MAP_UNKNOWN_LEN, fname, NULL);
    calc_len = limit == SHA1_LIMIT_WHOLE_FILE ? len : MIN(limit, len);
    xsha1((const unsigned char *) map, calc_len, sha1_raw);
    map_free(&map, &len);
    r = bin_to_hex(sha1_raw, SHA1_DIGEST_LENGTH, buf, BH_LOWER);
    assert(r == 2 * SHA1_DIGEST_LENGTH);

    return buf;
}

static int _append_start(struct backup *backup, time_t ts, off_t offset,
                         const char *file_sha1, int index_only, int noflush)
{
    if (backup->append_state != NULL) fatal("backup append already started", EC_SOFTWARE);

    struct backup_append_state *append_state = xzmalloc(sizeof(*append_state));

    if (index_only) append_state->mode |= BACKUP_APPEND_INDEXONLY;
    if (noflush) append_state->mode |= BACKUP_APPEND_NOFLUSH;

    SHA1_Init(&append_state->sha_ctx);

    char header[80];
    snprintf(header, sizeof(header), "# cyrus backup: chunk start %ld\r\n", (int64_t) ts);

    if (!index_only) {
        int dup_fd = dup(backup->fd);
        append_state->gzfile = gzdopen(dup_fd, "ab");
        if (!append_state->gzfile) {
            fprintf(stderr, "%s: gzdopen fd %i failed: %s\n", __func__, dup_fd, strerror(errno));
            return -1;
        }

        // FIXME check for error return
        gzwrite(append_state->gzfile, header, strlen(header));
        if (!noflush)
            gzflush(append_state->gzfile, Z_FULL_FLUSH);
    }

    SHA1_Update(&append_state->sha_ctx, header, strlen(header));
    append_state->wrote += strlen(header);

    struct sqldb_bindval bval[] = {
        { ":timestamp", SQLITE_INTEGER, { .i = ts           } },
        { ":offset",    SQLITE_INTEGER, { .i = offset       } },
        { ":file_sha1", SQLITE_TEXT,    { .s = file_sha1    } },
        { NULL,         SQLITE_NULL,    { .s = NULL         } },
    };

    sqldb_begin(backup->db, "backup_index"); // FIXME what if this fails

    int r = sqldb_exec(backup->db, backup_index_start_sql, bval, NULL, NULL);
    if (r) {
        // FIXME handle this sensibly
        fprintf(stderr, "%s: something went wrong: %i\n", __func__, r);
        sqldb_rollback(backup->db, "backup_index");
        goto error;
    }

    append_state->index_id = sqldb_lastid(backup->db);
    backup->append_state = append_state;
    return 0;

error:
    if (append_state) {
        if (append_state->gzfile)
            gzclose_w(append_state->gzfile);
        free(append_state);
    }
    return -1;
}

EXPORTED int backup_append_start(struct backup *backup)
{
    char file_sha1[2 * SHA1_DIGEST_LENGTH + 1];
    off_t offset = lseek(backup->fd, 0, SEEK_END);

    _sha1_file(backup->fd, backup->data_fname, SHA1_LIMIT_WHOLE_FILE, file_sha1);

    return _append_start(backup, time(0), offset, file_sha1, 0, 0);
}

EXPORTED int backup_append(struct backup *backup, struct dlist *dlist, time_t ts)
{
    int r;
    if (!backup->append_state) fatal("backup append not started", EC_SOFTWARE);

    off_t start = backup->append_state->wrote;
    size_t len;

    /* build a buffer containing the data to be written */
    struct buf buf = BUF_INITIALIZER, ts_buf = BUF_INITIALIZER;
    dlist_printbuf(dlist, 1, &buf);
    buf_printf(&ts_buf, "%ld APPLY ", (int64_t) ts);
    buf_insert(&buf, 0, &ts_buf);
    buf_appendcstr(&buf, "\r\n");

    /* track the sha1sum */
    SHA1_Update(&backup->append_state->sha_ctx, buf_cstring(&buf), buf_len(&buf));

    /* if we're not in index-only mode, write the data out */
    if (!(backup->append_state->mode & BACKUP_APPEND_INDEXONLY)) {
        /* gzprintf's internal buffer is limited to about 8K, which
         * dlist will exceed if there's a message in it, so use gzwrite
         * rather than gzprintf for writing the dlist contents.
         */
        const char *p = buf_cstring(&buf);
        size_t left = buf_len(&buf);

        while (left) {
            int n = MIN(left, INT32_MAX);
            int wrote = gzwrite(backup->append_state->gzfile, p, n);
            if (wrote > 0) {
                left -= wrote;
                p += wrote;
            }
            else {
                const char *err = gzerror(backup->append_state->gzfile, &r);
                syslog(LOG_ERR, "IOERROR: %s gzwrite %s: %s", __func__, backup->data_fname, err);

                if (r == Z_STREAM_ERROR)
                    fatal("gzwrite: invalid stream", EC_IOERR);
                else if (r == Z_MEM_ERROR)
                    fatal("gzwrite: out of memory", EC_TEMPFAIL);

                goto error;
            }
        }

        if (!(backup->append_state->mode & BACKUP_APPEND_NOFLUSH)) {
            r = gzflush(backup->append_state->gzfile, Z_FULL_FLUSH);
            if (r != Z_OK) {
                syslog(LOG_ERR, "IOERROR: %s gzflush %s: %i %i", __func__, backup->data_fname, r, errno);
                goto error;
            }
        }
    }

    /* count the written bytes */
    len = buf_len(&buf);
    backup->append_state->wrote += buf_len(&buf);

    buf_free(&buf);

    /* update the index */
    return backup_index(backup, dlist, start, len);

error:
    buf_free(&buf);
    return IMAP_INTERNAL;
}

int backup_append_end(struct backup *backup) {
    int r;
    struct backup_append_state *append_state = backup->append_state;

    backup->append_state = NULL;

    if (!append_state) fatal("backup append not started", EC_SOFTWARE);

    if (!(append_state->mode & BACKUP_APPEND_INDEXONLY)) {
        r = gzflush(append_state->gzfile, Z_FULL_FLUSH);
        if (!r) r = gzclose_w(append_state->gzfile);
        if (r != Z_OK) {
            fprintf(stderr, "%s: gzclose_w failed: %i\n", __func__, r);
            // FIXME handle this sensibly
        }
    }

    unsigned char sha1_raw[SHA1_DIGEST_LENGTH];
    char data_sha1[2 * SHA1_DIGEST_LENGTH + 1];
    SHA1_Final(sha1_raw, &append_state->sha_ctx);
    r = bin_to_hex(sha1_raw, SHA1_DIGEST_LENGTH, data_sha1, BH_LOWER);
    assert(r == 2 * SHA1_DIGEST_LENGTH);

    struct sqldb_bindval bval[] = {
        { ":id",        SQLITE_INTEGER, { .i = append_state->index_id   } },
        { ":length",    SQLITE_INTEGER, { .i = append_state->wrote      } },
        { ":data_sha1", SQLITE_TEXT,    { .s = data_sha1                } },
        { NULL,         SQLITE_NULL,    { .s = NULL                     } },
    };

    r = sqldb_exec(backup->db, backup_index_end_sql, bval, NULL, NULL);
    if (r) {
        // FIXME handle this sensibly
        fprintf(stderr, "%s: something went wrong: %i\n", __func__, r);
        sqldb_rollback(backup->db, "backup_index");
    }
    else {
        sqldb_commit(backup->db, "backup_index");
    }

    free(append_state);
    return r;
}

EXPORTED int backup_append_abort(struct backup *backup)
{
    struct backup_append_state *append_state = backup->append_state;

    backup->append_state = NULL;

    if (!append_state) fatal("backup append not started", EC_SOFTWARE);

    sqldb_rollback(backup->db, "backup_index");

    // FIXME
    // can we truncate back to the length we started this append at?
    // ftruncate(2) says nothing about behaviour on descriptors
    // opened with O_APPEND...
    // seems like it might work, but test it first.

    // FIXME at least close the damn file...

    free(append_state);
    return 0;
}

static int _parse_line(struct protstream *in, time_t *ts,
                       struct buf *cmd, struct dlist **kin)
{
    struct dlist *dl = NULL;
    struct buf buf = BUF_INITIALIZER;
    int64_t t;
    int c;

    c = prot_getc(in);
    if (c == '#')
        eatline(in, c);
    else
        prot_ungetc(c, in);

    c = getint64(in, &t);
    if (c == EOF)
        goto fail;

    c = getword(in, &buf);
    if (c == EOF)
        goto fail;

    c = dlist_parse(&dl, DLIST_SFILE | DLIST_PARSEKEY, in);

    if (!dl) {
        fprintf(stderr, "\ndidn't parse dlist, error %i\n", c);
        goto fail;
    }

    if (c == '\r') c = prot_getc(in);
    if (c != '\n') {
        fprintf(stderr, "expected newline, got '%c'\n", c);
        eatline(in, c);
        goto fail;
    }

    if (kin) *kin = dl;
    if (cmd) buf_copy(cmd, &buf);
    if (ts) *ts = (time_t) t;
    buf_free(&buf);
    return c;

fail:
    if (dl) dlist_free(&dl);
    buf_free(&buf);
    return c;
}

static ssize_t _prot_fill_cb(unsigned char *buf, size_t len, void *rock)
{
    struct gzuncat *gzuc = (struct gzuncat *) rock;
    return gzuc_read(gzuc, buf, len);
}

EXPORTED int backup_reindex(const char *name)
{
    struct buf data_fname = BUF_INITIALIZER;
    struct buf index_fname = BUF_INITIALIZER;
    int r;

    buf_printf(&data_fname, "%s", name);
    buf_printf(&index_fname, "%s.index", name);

    struct backup *backup = _open_internal(buf_cstring(&data_fname),
                                           buf_cstring(&index_fname),
                                           BACKUP_OPEN_REINDEX);
    buf_free(&index_fname);
    buf_free(&data_fname);
    if (!backup) return -1;

    struct gzuncat *gzuc = gzuc_open(backup->fd);

    time_t prev_member_ts = -1;

    while (gzuc && !gzuc_eof(gzuc)) {
        gzuc_member_start(gzuc);
        off_t member_offset = gzuc_member_offset(gzuc);

        fprintf(stderr, "\nfound chunk at offset %jd\n\n", member_offset);

        struct protstream *member = prot_readcb(_prot_fill_cb, gzuc);
        prot_setisclient(member, 1); /* don't sync literals */

        // FIXME stricter timestamp sequence checks
        time_t member_ts = -1;

        while (1) {
            struct buf cmd = BUF_INITIALIZER;
            time_t ts;
            struct dlist *dl = NULL;

            int c = _parse_line(member, &ts, &cmd, &dl);
            if (c == EOF) break;

            if (member_ts == -1) {
                if (prev_member_ts != -1 && prev_member_ts > ts) {
                    fatal("member timestamp older than previous", EC_DATAERR);
                }
                member_ts = ts;
                char file_sha1[2 * SHA1_DIGEST_LENGTH + 1];
                _sha1_file(backup->fd, backup->data_fname, member_offset, file_sha1);
                _append_start(backup, member_ts, member_offset, file_sha1, 1, 0);
            }
            else if (member_ts > ts)
                fatal("line timestamp older than previous", EC_DATAERR);

            if (strcmp(buf_cstring(&cmd), "APPLY") != 0)
                continue;

            ucase(dl->name);

            r = backup_append(backup, dl, ts);
            if (r) {
                // FIXME do something
            }
        }

        backup_append_end(backup);
        prot_free(member);
        gzuc_member_end(gzuc, NULL);

        prev_member_ts = member_ts;
    }

    fprintf(stderr, "reached end of file\n");

    gzuc_close(&gzuc);
    backup_close(&backup);

    return r;
}