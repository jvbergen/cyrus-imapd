/* indexr.c -- replication-based backup api - index reading functions
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
#include <syslog.h>

#include "lib/xmalloc.h"

#include "backup/api.h"
#include "backup/sqlconsts.h"

#define BACKUP_INTERNAL_SOURCE /* this file is part of the backup API */
#include "backup/internal.h"


/* FIXME do this properly */
int _column_int(sqlite3_stmt *stmt, int column);
sqlite3_int64 _column_int64(sqlite3_stmt *stmt, int column);
char * _column_text(sqlite3_stmt *stmt, int column);
/***********************************************/


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

static void backup_mailbox_message_list_add(
    struct backup_mailbox_message_list *list,
    struct backup_mailbox_message *mailbox_message)
{
    mailbox_message->next = NULL;

    if (!list->head)
        list->head = mailbox_message;

    if (list->tail)
        list->tail->next = mailbox_message;

    list->tail = mailbox_message;

    list->count++;
}

HIDDEN struct backup_mailbox_message *backup_mailbox_message_list_remove(
    struct backup_mailbox_message_list *list,
    struct backup_mailbox_message *mailbox_message)
{
    struct backup_mailbox_message *node, *prev;

    assert(list != NULL);
    assert(mailbox_message != NULL);

    prev = NULL;
    node = list->head;
    while (node && node != mailbox_message) {
        prev = node;
        node = node->next;
    }

    if (!node) return NULL;
    assert(node == mailbox_message);

    if (prev) {
        prev->next = node->next;
    }
    else {
        assert(node == list->head);
        list->head = node->next;
    }

    if (!node->next) {
        assert(node == list->tail);
        list->tail = prev;
    }

    node->next = NULL;
    list->count--;
    return node;
}

EXPORTED void backup_mailbox_message_list_empty(
    struct backup_mailbox_message_list *list)
{
    struct backup_mailbox_message *mailbox_message, *next;

    mailbox_message = list->head;
    while (mailbox_message) {
        next = mailbox_message->next;
        backup_mailbox_message_free(&mailbox_message);
        mailbox_message = next;
    }

    memset(list, 0, sizeof(*list));
}

static void backup_mailbox_list_add(struct backup_mailbox_list *list,
                                    struct backup_mailbox *mailbox)
{
    mailbox->next = NULL;

    if (!list->head)
        list->head = mailbox;

    if (list->tail)
        list->tail->next = mailbox;

    list->tail = mailbox;

    list->count++;
}

HIDDEN struct backup_mailbox *backup_mailbox_list_remove(
    struct backup_mailbox_list *list,
    struct backup_mailbox *mailbox)
{
    struct backup_mailbox *node, *prev;

    assert(list != NULL);
    assert(mailbox != NULL);

    prev = NULL;
    node = list->head;
    while (node && node != mailbox) {
        prev = node;
        node = node->next;
    }

    if (!node) return NULL;
    assert(node == mailbox);

    if (prev) {
        prev->next = node->next;
    }
    else {
        assert(node == list->head);
        list->head = node->next;
    }

    if (!node->next) {
        assert(node == list->tail);
        list->tail = prev;
    }

    node->next = NULL;
    list->count--;
    return node;
}

EXPORTED void backup_mailbox_list_empty(struct backup_mailbox_list *list)
{
    struct backup_mailbox *mailbox, *next;

    mailbox = list->head;
    while (mailbox) {
        next = mailbox->next;
        backup_mailbox_free(&mailbox);
        mailbox = next;
    }

    memset(list, 0, sizeof(*list));
}

static int _mailbox_message_row_cb(sqlite3_stmt *stmt, void *rock)
{
    struct backup_mailbox_message_list *save_list;
    struct backup_mailbox_message *mailbox_message;
    char *guid_str = NULL;
    int r = 0;

    save_list = (struct backup_mailbox_message_list *) rock;
    mailbox_message = xzmalloc(sizeof *mailbox_message);

    int column = 0;
    mailbox_message->id = _column_int(stmt, column++);
    mailbox_message->mailbox_id = _column_int(stmt, column++);
    mailbox_message->mailbox_uniqueid = _column_text(stmt, column++);
    mailbox_message->message_id = _column_int(stmt, column++);
    mailbox_message->last_chunk_id = _column_int(stmt, column++);
    mailbox_message->uid = _column_int(stmt, column++);
    mailbox_message->modseq = _column_int64(stmt, column++);
    mailbox_message->last_updated = _column_int64(stmt, column++);
    mailbox_message->flags = _column_text(stmt, column++);
    mailbox_message->internaldate = _column_int64(stmt, column++);
    guid_str = _column_text(stmt, column++);
    mailbox_message->size = _column_int(stmt, column++);
    mailbox_message->annotations = _column_text(stmt, column++);
    mailbox_message->expunged = _column_int(stmt, column++);

    message_guid_decode(&mailbox_message->guid, guid_str);
    free(guid_str);

    if (save_list)
        backup_mailbox_message_list_add(save_list, mailbox_message);
    else
        backup_mailbox_message_free(&mailbox_message);

    return r;
}

EXPORTED struct backup_mailbox_message_list *backup_get_mailbox_messages(
    struct backup *backup,
    int chunk_id)
{
    struct backup_mailbox_message_list *mailbox_message_list =
        xzmalloc(sizeof *mailbox_message_list);

    struct sqldb_bindval bval[] = {
        { ":last_chunk_id", SQLITE_INTEGER, { .i = chunk_id } },
        { NULL,             SQLITE_NULL,    { .s = NULL } },
    };

    const char *sql = chunk_id ?
        backup_index_mailbox_message_select_chunkid_sql :
        backup_index_mailbox_message_select_all_sql;

    int r = sqldb_exec(backup->db, sql, bval, _mailbox_message_row_cb,
                       mailbox_message_list);

    if (r) {
        backup_mailbox_message_list_empty(mailbox_message_list);
        free(mailbox_message_list);
        return NULL;
    }

    return mailbox_message_list;
}

struct _mailbox_row_rock {
    sqldb_t *db;
    backup_mailbox_foreach_cb proc;
    void *rock;
    struct backup_mailbox_list *save_list;
    struct backup_mailbox **save_one;
    int want_records;
};

static int _mailbox_row_cb(sqlite3_stmt *stmt, void *rock)
{
    struct _mailbox_row_rock *mbrock = (struct _mailbox_row_rock *) rock;
    struct backup_mailbox *mailbox = xzmalloc(sizeof *mailbox);
    int r = 0;

    int column = 0;
    mailbox->id = _column_int(stmt, column++);
    mailbox->last_chunk_id = _column_int(stmt, column++);
    mailbox->uniqueid = _column_text(stmt, column++);
    mailbox->mboxname = _column_text(stmt, column++);
    mailbox->mboxtype = _column_text(stmt, column++);
    mailbox->last_uid = _column_int(stmt, column++);
    mailbox->highestmodseq = _column_int64(stmt, column++);
    mailbox->recentuid = _column_int(stmt, column++);
    mailbox->recenttime = _column_int64(stmt, column++);
    mailbox->last_appenddate = _column_int64(stmt, column++);
    mailbox->pop3_last_login = _column_int64(stmt, column++);
    mailbox->pop3_show_after = _column_int64(stmt, column++);
    mailbox->uidvalidity = _column_int(stmt, column++);
    mailbox->partition = _column_text(stmt, column++);
    mailbox->acl = _column_text(stmt, column++);
    mailbox->options = _column_text(stmt, column++);
    mailbox->sync_crc = _column_int(stmt, column++);
    mailbox->sync_crc_annot = _column_int(stmt, column++);
    mailbox->quotaroot = _column_text(stmt, column++);
    mailbox->xconvmodseq = _column_int64(stmt, column++);
    mailbox->annotations = _column_text(stmt, column++);
    mailbox->deleted = _column_int64(stmt, column++);

    if (mbrock->want_records) {
        struct backup_mailbox_message_list *records =
            xzmalloc(sizeof *mailbox->records);

        struct sqldb_bindval bval[] = {
            { ":mailbox_id",    SQLITE_INTEGER, { .i = mailbox->id } },
            { NULL,             SQLITE_NULL,    { .s = NULL } },
        };

        r = sqldb_exec(mbrock->db,
                       backup_index_mailbox_message_select_mailbox_sql,
                       bval,
                       _mailbox_message_row_cb, mailbox->records);

        if (r) {
            backup_mailbox_message_list_empty(records);
            free(records);
        }
        else {
            mailbox->records = records;
        }

        // FIXME sensible error handling
    }

    if (mbrock->proc)
        r = mbrock->proc(mailbox, mbrock->rock);

    if (mbrock->save_list)
        backup_mailbox_list_add(mbrock->save_list, mailbox);
    else if (mbrock->save_one)
        *mbrock->save_one = mailbox;
    else
        backup_mailbox_free(&mailbox);

    return r;
}

EXPORTED int backup_mailbox_foreach(struct backup *backup,
                                    int chunk_id,
                                    int want_records,
                                    backup_mailbox_foreach_cb cb,
                                    void *rock)
{
    struct _mailbox_row_rock mbrock = { backup->db, cb, rock,
                                        NULL, NULL, want_records};

    struct sqldb_bindval bval[] = {
        { ":last_chunk_id", SQLITE_INTEGER, { .i = chunk_id } },
        { NULL,             SQLITE_NULL,    { .s = NULL } },
    };

    const char *sql = chunk_id ?
        backup_index_mailbox_select_chunkid_sql :
        backup_index_mailbox_select_all_sql;

    int r = sqldb_exec(backup->db, sql, bval, _mailbox_row_cb, &mbrock);

    return r;
}

EXPORTED struct backup_mailbox_list *backup_get_mailboxes(struct backup *backup,
                                                          int chunk_id,
                                                          int want_records)
{
    struct backup_mailbox_list *mailbox_list = xzmalloc(sizeof *mailbox_list);

    struct _mailbox_row_rock mbrock = { backup->db, NULL, NULL,
                                        mailbox_list, NULL, want_records};

    struct sqldb_bindval bval[] = {
        { ":last_chunk_id", SQLITE_INTEGER, { .i = chunk_id } },
        { NULL,             SQLITE_NULL,    { .s = NULL } },
    };

    const char *sql = chunk_id ?
        backup_index_mailbox_select_chunkid_sql :
        backup_index_mailbox_select_all_sql;

    int r = sqldb_exec(backup->db, sql, bval, _mailbox_row_cb, &mbrock);

    if (r) {
        backup_mailbox_list_empty(mailbox_list);
        free(mailbox_list);
        return NULL;
    }

    return mailbox_list;
}

EXPORTED struct backup_mailbox *backup_get_mailbox_by_name(struct backup *backup,
                                                  const mbname_t *mbname,
                                                  int want_records)
{
    struct backup_mailbox *mailbox = NULL;

    struct _mailbox_row_rock mbrock = { backup->db, NULL, NULL,
                                        NULL, &mailbox, want_records };

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

EXPORTED struct dlist *backup_mailbox_to_dlist(
    const struct backup_mailbox *mailbox)
{
    struct dlist *dl = dlist_newkvlist(NULL, "MAILBOX");
    int r;

    dlist_setatom(dl, "UNIQUEID", mailbox->uniqueid);
    dlist_setatom(dl, "MBOXNAME", mailbox->mboxname);
    dlist_setatom(dl, "MBOXTYPE", mailbox->mboxtype);
    dlist_setnum32(dl, "LAST_UID", mailbox->last_uid);
    dlist_setnum64(dl, "HIGHESTMODSEQ", mailbox->highestmodseq);
    dlist_setnum32(dl, "RECENTUID", mailbox->recentuid);
    dlist_setdate(dl, "RECENTTIME", mailbox->recenttime);
    dlist_setdate(dl, "LAST_APPENDDATE", mailbox->last_appenddate);
    dlist_setdate(dl, "POP3_LAST_LOGIN", mailbox->pop3_last_login);
    dlist_setdate(dl, "POP3_SHOW_AFTER", mailbox->pop3_show_after);
    dlist_setnum32(dl, "UIDVALIDITY", mailbox->uidvalidity);
    dlist_setatom(dl, "PARTITION", mailbox->partition);
    dlist_setatom(dl, "ACL", mailbox->acl);
    dlist_setatom(dl, "OPTIONS", mailbox->options);
    dlist_setnum32(dl, "SYNC_CRC", mailbox->sync_crc);
    dlist_setnum32(dl, "SYNC_CRC_ANNOT", mailbox->sync_crc_annot);
    dlist_setatom(dl, "QUOTAROOT", mailbox->quotaroot);
    dlist_setnum64(dl, "XCONVMODSEQ", mailbox->xconvmodseq);

    if (mailbox->annotations) {
        struct dlist *annots = NULL;
        r = dlist_parsemap(&annots, 0, mailbox->annotations,
                           strlen(mailbox->annotations));
        // FIXME handle error sanely
        if (annots) {
            annots->name = xstrdup("ANNOTATIONS");
            dlist_stitch(dl, annots);
        }
    }

    if (mailbox->records && mailbox->records->count) {
        struct dlist *records = dlist_newlist(NULL, "RECORD");
        struct backup_mailbox_message *mailbox_message = mailbox->records->head;

        while (mailbox_message) {
            struct dlist *record = dlist_newkvlist(records, NULL);

            dlist_setnum32(record, "UID", mailbox_message->uid);
            dlist_setnum64(record, "MODSEQ", mailbox_message->modseq);
            dlist_setdate(record, "LAST_UPDATED", mailbox_message->last_updated);
            dlist_setdate(record, "INTERNALDATE", mailbox_message->internaldate);
            dlist_setguid(record, "GUID", &mailbox_message->guid);
            dlist_setnum32(record, "SIZE", mailbox_message->size);

            if (mailbox_message->flags) {
                struct dlist *flags = NULL;
                r = dlist_parsemap(&flags, 0, mailbox_message->flags,
                                   strlen(mailbox_message->flags));
                // FIXME handle error sanely
                if (flags) {
                    flags->name = xstrdup("FLAGS");
                    if (mailbox_message->expunged)
                        dlist_setflag(flags, "FLAG", "\\Expunged");
                    dlist_stitch(record, flags);
                }
            }

            if (mailbox_message->annotations) {
                struct dlist *annots = NULL;
                r = dlist_parsemap(&annots, 0, mailbox_message->annotations,
                                   strlen(mailbox_message->annotations));
                // FIXME handle error sanely
                if (annots)  {
                    annots->name = xstrdup("ANNOTATIONS");
                    dlist_stitch(record, annots);
                }
            }

            mailbox_message = mailbox_message->next;
        }

        dlist_stitch(dl, records);
    }

    // FIXME check for error
    (void) r;

    return dl;
}

EXPORTED void backup_mailbox_message_free(
    struct backup_mailbox_message **mailbox_messagep)
{
    struct backup_mailbox_message *mailbox_message = *mailbox_messagep;
    *mailbox_messagep = NULL;

    if (mailbox_message->flags) free(mailbox_message->flags);
    if (mailbox_message->annotations) free(mailbox_message->annotations);
    if (mailbox_message->mailbox_uniqueid) free(mailbox_message->mailbox_uniqueid);

    free(mailbox_message);
}

EXPORTED void backup_mailbox_free(struct backup_mailbox **mailboxp)
{
    struct backup_mailbox *mailbox = *mailboxp;
    *mailboxp = NULL;

    if (mailbox->uniqueid) free(mailbox->uniqueid);
    if (mailbox->mboxname) free(mailbox->mboxname);
    if (mailbox->mboxtype) free(mailbox->mboxtype);
    if (mailbox->partition) free(mailbox->partition);
    if (mailbox->acl) free(mailbox->acl);
    if (mailbox->options) free(mailbox->options);
    if (mailbox->quotaroot) free(mailbox->quotaroot);
    if (mailbox->annotations) free(mailbox->annotations);

    if (mailbox->records) {
        backup_mailbox_message_list_empty(mailbox->records);
        free(mailbox->records);
    }

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
    message->chunk_id = _column_int(stmt, column++);
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

struct message_row_rock {
    backup_message_foreach_cb proc;
    void *rock;
};

static int _message_row_cb(sqlite3_stmt *stmt, void *rock)
{
    struct message_row_rock *mrock = (struct message_row_rock *) rock;
    struct backup_message message = {0};
    int column = 0;
    int r = 0;

    message.id = _column_int(stmt, column++);
    char *guid_str = _column_text(stmt, column++);
    message.partition = _column_text(stmt, column++);
    message.chunk_id = _column_int(stmt, column++);
    message.offset = _column_int64(stmt, column++);
    message.length = _column_int64(stmt, column++);

    message.guid = xzmalloc(sizeof *message.guid);
    if (!message_guid_decode(message.guid, guid_str))
        r = -1;
    free(guid_str);

    if (!r)
        r = mrock->proc(&message, mrock->rock);

    if (message.guid) free(message.guid);
    if (message.partition) free(message.partition);
    memset(&message, 0, sizeof message);

    return r;
}

EXPORTED int backup_message_foreach(struct backup *backup, int chunk_id,
                                    backup_message_foreach_cb cb, void *rock)
{
    struct sqldb_bindval bval[] = {
        { ":chunk_id",  SQLITE_INTEGER, { .i = chunk_id } },
        { NULL,         SQLITE_NULL,    { .s = NULL } },
    };

    struct message_row_rock mrock = { cb, rock };

    const char *sql = chunk_id ?
        backup_index_message_select_chunkid_sql :
        backup_index_message_select_all_sql;

    return sqldb_exec(backup->db, sql, bval, _message_row_cb, &mrock);
}

EXPORTED void backup_chunk_list_add(struct backup_chunk_list *list,
                                    struct backup_chunk *chunk)
{
    /* n.b. always inserts at head */
    chunk->next = list->head;
    list->head = chunk;
    if (!list->tail)
        list->tail = chunk;

    list->count++;
}

EXPORTED void backup_chunk_list_empty(struct backup_chunk_list *list)
{
    struct backup_chunk *curr, *next;
    curr = list->head;
    while (curr) {
        next = curr->next;
        backup_chunk_free(&curr);
        curr = next;
    }

    list->head = list->tail = NULL;
    list->count = 0;
}

EXPORTED void backup_chunk_list_free(struct backup_chunk_list **chunk_listp)
{
    struct backup_chunk_list *chunk_list = *chunk_listp;
    *chunk_listp = NULL;

    backup_chunk_list_empty(chunk_list);
    free(chunk_list);
}

struct _chunk_row_rock {
    struct backup_chunk_list *save_list;
    struct backup_chunk **save_one;
};

static int _chunk_row_cb(sqlite3_stmt *stmt, void *rock)
{
    struct _chunk_row_rock *crock = (struct _chunk_row_rock *) rock;

    struct backup_chunk *chunk = xzmalloc(sizeof(*chunk));

    int column = 0;
    chunk->id = _column_int(stmt, column++);
    chunk->ts_start = _column_int64(stmt, column++);
    chunk->ts_end = _column_int64(stmt, column++);
    chunk->offset = _column_int64(stmt, column++);
    chunk->length = _column_int64(stmt, column++);
    chunk->file_sha1 = _column_text(stmt, column++);
    chunk->data_sha1 = _column_text(stmt, column++);

    if (crock->save_list) {
        backup_chunk_list_add(crock->save_list, chunk);
    }
    else if (crock->save_one) {
        *crock->save_one = chunk;
    }
    else {
        syslog(LOG_DEBUG, "%s: useless invocation with nowhere to save to", __func__);
        backup_chunk_free(&chunk);
    }

    return 0;
}

EXPORTED struct backup_chunk_list *backup_get_chunks(struct backup *backup)
{
    struct backup_chunk_list *chunk_list = xzmalloc(sizeof *chunk_list);

    struct _chunk_row_rock crock = { chunk_list, NULL };

    int r = sqldb_exec(backup->db, backup_index_chunk_select_all_sql,
                       NULL, _chunk_row_cb, &crock);

    if (r) {
        backup_chunk_list_free(&chunk_list);
        return NULL;
    }

    return chunk_list;
}

EXPORTED struct backup_chunk *backup_get_latest_chunk(struct backup *backup)
{
    struct backup_chunk *chunk = NULL;
    struct _chunk_row_rock crock = { NULL, &chunk };

    int r = sqldb_exec(backup->db, backup_index_chunk_select_latest_sql,
                       NULL, _chunk_row_cb, &crock);

    if (r) {
        if (chunk) backup_chunk_free(&chunk);
        return NULL;
    }

    return chunk;
}

EXPORTED void backup_chunk_free(struct backup_chunk **chunkp)
{
    struct backup_chunk *chunk = *chunkp;
    *chunkp = NULL;

    if (chunk->file_sha1) free(chunk->file_sha1);
    if (chunk->data_sha1) free(chunk->data_sha1);

    free(chunk);
}