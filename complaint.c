/*
 * Player complaint system.
 *
 * Replaces the old sendmail-based /complain handling: complaints are stored in
 * the 'complaints' database table together with a pre-rendered, HTML-escaped
 * copy of the complainer's chat scrollback. Staff review them in-game with
 * /complaints (see command.c); the stored HTML is streamed to the staff client
 * through the SV_CHATLOG protocol message (see tick_chatlog_xfer in player.c).
 *
 * Threading: complaint_new() runs on the main thread (it reads the player
 * scrollback and only queues a query). All db_complaint_*() functions run on
 * the database background thread, following the db_karmalog()/set_task()
 * patterns: results go to the requesting character via tell_chat(), and game
 * state is only touched under lock_server().
 */

#define _GNU_SOURCE // for strcasestr

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <zlib.h>
#include <mysql/mysql.h>

int mysql_query_con(MYSQL *my, const char *query);
void mysql_free_result_cnt(MYSQL_RES *result);
MYSQL_RES *mysql_store_result_cnt(MYSQL *my);

#include "server.h"
#include "client.h"
#include "log.h"
#include "mem.h"
#include "database.h"
#include "create.h"
#include "chat.h"
#include "lookup.h"
#include "complaint.h"

#define NEED_PLAYER_STRUCT
#include "player.h"
#undef NEED_PLAYER_STRUCT

extern MYSQL mysql;

// Worst case every log byte escapes to '&quot;' (6 chars); the log appears
// twice (summary + full log) plus template and header overhead.
#define COMPLAINT_HTML_MAX (MAXSCROLLBACK * 12 + 4096)

// Append src to dst as HTML-escaped text, dropping the \260cN color codes and
// mapping other control characters to spaces. Never writes past max-1; always
// leaves dst zero-terminated. Returns the new length.
static int html_escape_append(char *dst, int pos, int max, const char *src) {
    unsigned char c;

    while ((c = (unsigned char)*src)) {
        if (c == 0260) { // color/link code: \260 'c' digits
            src++;
            if (*src == 'c') {
                src++;
                while (isdigit((unsigned char)*src)) src++;
            }
            continue;
        }
        if (c < 32) { // stray control chars (client position codes etc.)
            c = ' ';
        }

        if (pos >= max - 8) break; // leave room for the longest entity + zero

        switch (c) {
        case '&':
            pos += sprintf(dst + pos, "&amp;");
            break;
        case '<':
            pos += sprintf(dst + pos, "&lt;");
            break;
        case '>':
            pos += sprintf(dst + pos, "&gt;");
            break;
        case '"':
            pos += sprintf(dst + pos, "&quot;");
            break;
        case '\'':
            pos += sprintf(dst + pos, "&#39;");
            break;
        default:
            dst[pos++] = (char)c;
            break;
        }
        src++;
    }
    dst[pos] = 0;

    return pos;
}

// Walk the scrollback ring buffer and append each line. If matcha/matchb are
// set, only lines containing one of them are appended (summary mode). Mirrors
// the loops in write_scrollback (player.c).
static int append_scrollback(char *dst, int pos, int max, int nr, char *matcha, char *matchb) {
    char line[MAXSCROLLBACK + 16];
    int n, lp = 0, start = 0;

    for (n = player[nr]->scrollpos + 1; n < MAXSCROLLBACK; n++) {
        if (player[nr]->scrollback[n]) {
            line[lp++] = player[nr]->scrollback[n];
            start = 1;
        } else if (start) {
            line[lp] = 0;
            lp = 0;
            if (!matcha || strcasestr(line, matcha) || strcasestr(line, matchb)) {
                pos = html_escape_append(dst, pos, max, line);
                if (pos < max - 2) pos += sprintf(dst + pos, "\n");
            }
        }
    }
    for (n = 0; n < player[nr]->scrollpos; n++) {
        if (player[nr]->scrollback[n]) line[lp++] = player[nr]->scrollback[n];
        else {
            line[lp] = 0;
            lp = 0;
            if (!matcha || strcasestr(line, matcha) || strcasestr(line, matchb)) {
                pos = html_escape_append(dst, pos, max, line);
                if (pos < max - 2) pos += sprintf(dst + pos, "\n");
            }
        }
    }

    return pos;
}

// Build the complaint HTML and queue the database insert. Runs on the main
// thread. cn = complainer, vID/vname = offender, reason = text after the name.
int complaint_new(int cn, int vID, char *vname, char *reason) {
    char *html, *esc, *query;
    char reason_raw[256], reason_esc[512], cname_esc[128], vname_esc[128], announce[512];
    struct tm *tm;
    time_t t;
    int pos, nr, ret;

    nr = ch[cn].player;
    if (nr < 1 || nr >= MAXPLAYER || !player[nr]) return 0;

    if (!*reason) reason = "(none)";
    snprintf(reason_raw, 200, "%s", reason); // fits varchar(255) after escaping

    html = xmalloc(COMPLAINT_HTML_MAX, IM_TEMP);
    if (!html) return 0;

    t = time_now;
    tm = localtime(&t);

    // header (names/reason are escaped into the fixed-size bufs first)
    html_escape_append(cname_esc, 0, 60, ch[cn].name);
    html_escape_append(vname_esc, 0, 60, vname);
    pos = sprintf(html,
                  "<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
                  "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'\">\n"
                  "<title>Complaint from %s about %s</title>\n"
                  "<style>\n"
                  "body { background: #1a1a22; color: #ccc; font-family: sans-serif; margin: 2em; }\n"
                  "h1 { color: #e0b040; font-size: 1.3em; }\n"
                  "h2 { color: #8ab; font-size: 1.1em; margin-top: 1.5em; }\n"
                  "pre { background: #10101a; padding: 1em; overflow-x: auto; white-space: pre-wrap; }\n"
                  "</style>\n</head>\n<body>\n"
                  "<h1>Complaint from %s about %s</h1>\n",
                  cname_esc, vname_esc, cname_esc, vname_esc);

    pos += sprintf(html + pos, "<p>Date: %02d:%02d:%02d on %d/%d/%02d</p>\n",
                   tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_mday, tm->tm_mon + 1, tm->tm_year - 100);

    pos += sprintf(html + pos, "<p>Reason: ");
    pos = html_escape_append(html, pos, COMPLAINT_HTML_MAX, reason_raw);
    pos += sprintf(html + pos, "</p>\n<h2>Summary (lines mentioning either player)</h2>\n<pre>");
    pos = append_scrollback(html, pos, COMPLAINT_HTML_MAX - 64, nr, ch[cn].name, vname);
    pos += sprintf(html + pos, "</pre>\n<h2>Whole log</h2>\n<pre>");
    pos = append_scrollback(html, pos, COMPLAINT_HTML_MAX - 64, nr, NULL, NULL);
    pos += sprintf(html + pos, "</pre>\n</body>\n</html>\n");

    // escape for SQL and build the insert
    esc = xmalloc(pos * 2 + 1, IM_TEMP);
    query = xmalloc(pos * 2 + 1024, IM_TEMP);
    if (!esc || !query) {
        if (esc) xfree(esc);
        if (query) xfree(query);
        xfree(html);
        return 0;
    }

    mysql_real_escape_string(&mysql, esc, html, strlen(html));
    mysql_real_escape_string(&mysql, reason_esc, reason_raw, strlen(reason_raw));

    sprintf(query, "insert into complaints values(0,%d,%d,'%s',%d,'%s','%s',0,0,0,'%s')",
            (int)time_now, ch[cn].ID, ch[cn].name, vID, vname, reason_esc, esc);

    // metadata for the staff channel announcement (names are alpha-only)
    sprintf(announce, "%s %s %.80s", ch[cn].name, vname, reason_raw);

    ret = queue_complaint_add(query, announce);

    xfree(query);
    xfree(esc);
    xfree(html);

    return ret;
}

// ---- database thread side --------------------------------------------------

// opt1 = insert query, opt2 = "cname vname reason..."
void db_complaint_add(char *query, char *announce) {
    char cname[80], vname[80], buf[256];
    char *reason;
    int id, n;

    if (mysql_query_con(&mysql, query)) {
        elog("Failed to insert complaint: Error: %s (%d)", mysql_error(&mysql), mysql_errno(&mysql));
        return;
    }
    id = (int)mysql_insert_id(&mysql);

    // unpack "cname vname reason..."
    for (n = 0; *announce && *announce != ' ' && n < 79; n++) cname[n] = *announce++;
    cname[n] = 0;
    while (*announce == ' ') announce++;
    for (n = 0; *announce && *announce != ' ' && n < 79; n++) vname[n] = *announce++;
    vname[n] = 0;
    while (*announce == ' ') announce++;
    reason = announce;

    snprintf(buf, 200, "0000000000\260c03New complaint #%d from %s about %s: \"%.40s\" \260c4/complaints view %d\260c0",
             id, cname, vname, reason, id);
    server_chat(31, buf);

    xlog("complaint #%d added (%s about %s)", id, cname, vname);
}

// opt = "staffID all"
void db_complaint_list(char *opt) {
    char buf[512];
    MYSQL_RES *result;
    MYSQL_ROW row;
    int staffID, all, id, date, state, cnt = 0;
    struct tm *tm;
    time_t t;

    staffID = atoi(opt);
    all = atoi(strchr(opt, ' ') ? strchr(opt, ' ') + 1 : "0");

    if (all) sprintf(buf, "select ID,date,cname,vname,reason,state from complaints order by ID desc limit 30");
    else sprintf(buf, "select ID,date,cname,vname,reason,state from complaints where state=0 order by ID desc limit 20");

    if (mysql_query_con(&mysql, buf)) {
        elog("Failed to read complaints: Error: %s (%d)", mysql_error(&mysql), mysql_errno(&mysql));
        return;
    }
    if (!(result = mysql_store_result_cnt(&mysql))) {
        elog("Failed to store result: Error: %s (%d)", mysql_error(&mysql), mysql_errno(&mysql));
        return;
    }

    tell_chat(0, staffID, 1, "%s complaints:", all ? "All" : "Open");

    while ((row = mysql_fetch_row(result))) {
        if (!row[0] || !row[1] || !row[2] || !row[3] || !row[4] || !row[5]) continue;

        id = atoi(row[0]);
        date = atoi(row[1]);
        state = atoi(row[5]);

        t = date;
        tm = localtime(&t);

        tell_chat(0, staffID, 1, "#%d %s about %s (%02d/%02d %02d:%02d)%s: \"%.80s\"",
                  id, row[2], row[3],
                  tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min,
                  state ? " [closed]" : "", row[4]);
        if (state) tell_chat(0, staffID, 1, "  \260c4/complaints view %d\260c0 \260c4/complaints log %d\260c0", id, id);
        else tell_chat(0, staffID, 1, "  \260c4/complaints view %d\260c0 \260c4/complaints log %d\260c0 \260c4/complaints close %d\260c0", id, id, id);
        cnt++;
    }
    if (!cnt) tell_chat(0, staffID, 1, "None. All quiet.");
    else tell_chat(0, staffID, 1, "End of complaints.");

    mysql_free_result_cnt(result);
}

// opt = "staffID id"
void db_complaint_view(char *opt) {
    char buf[512], closer_name[80];
    MYSQL_RES *result;
    MYSQL_ROW row;
    int staffID, id, state, closer;
    struct tm *tm;
    time_t t;

    staffID = atoi(opt);
    id = atoi(strchr(opt, ' ') ? strchr(opt, ' ') + 1 : "0");

    sprintf(buf, "select date,cID,cname,vID,vname,reason,state,closer,close_date from complaints where ID=%d", id);

    if (mysql_query_con(&mysql, buf)) {
        elog("Failed to read complaint %d: Error: %s (%d)", id, mysql_error(&mysql), mysql_errno(&mysql));
        return;
    }
    if (!(result = mysql_store_result_cnt(&mysql))) {
        elog("Failed to store result: Error: %s (%d)", mysql_error(&mysql), mysql_errno(&mysql));
        return;
    }

    if (!(row = mysql_fetch_row(result)) || !row[0] || !row[2] || !row[4] || !row[5] || !row[6]) {
        tell_chat(0, staffID, 1, "Complaint #%d not found.", id);
        mysql_free_result_cnt(result);
        return;
    }

    state = atoi(row[6]);
    t = atoi(row[0]);
    tm = localtime(&t);

    tell_chat(0, staffID, 1, "Complaint #%d from %s (ID %s) about %s (ID %s), %02d/%02d/%02d %02d:%02d.",
              id, row[2], row[1] ? row[1] : "?", row[4], row[3] ? row[3] : "?",
              tm->tm_mon + 1, tm->tm_mday, tm->tm_year - 100, tm->tm_hour, tm->tm_min);
    tell_chat(0, staffID, 1, "Reason: \"%.160s\"", row[5]);

    if (state) {
        closer = row[7] ? atoi(row[7]) : 0;
        lookup_ID(closer_name, closer);
        t = row[8] ? atoi(row[8]) : 0;
        tm = localtime(&t);
        tell_chat(0, staffID, 1, "Closed by %s on %02d/%02d/%02d %02d:%02d.",
                  closer_name, tm->tm_mon + 1, tm->tm_mday, tm->tm_year - 100, tm->tm_hour, tm->tm_min);
        tell_chat(0, staffID, 1, "  \260c4/complaints log %d\260c0", id);
    } else {
        tell_chat(0, staffID, 1, "  \260c4/complaints log %d\260c0 \260c4/complaints close %d\260c0", id, id);
        tell_chat(0, staffID, 1, "  \260c4/punish %s 1 complaint #%d\260c0 \260c4/shutup %s 10\260c0", row[4], id, row[4]);
    }

    mysql_free_result_cnt(result);
}

// opt = "staffID id"
void db_complaint_close(char *opt) {
    char buf[256];
    int staffID, id;

    staffID = atoi(opt);
    id = atoi(strchr(opt, ' ') ? strchr(opt, ' ') + 1 : "0");

    sprintf(buf, "update complaints set state=1, closer=%d, close_date=%d where ID=%d and state=0",
            staffID, (int)time_now, id);

    if (mysql_query_con(&mysql, buf)) {
        elog("Failed to close complaint %d: Error: %s (%d)", id, mysql_error(&mysql), mysql_errno(&mysql));
        return;
    }

    if (mysql_affected_rows(&mysql) == 1) tell_chat(0, staffID, 1, "Complaint #%d closed.", id);
    else tell_chat(0, staffID, 1, "Complaint #%d not found or already closed.", id);
}

// opt = "staffID id". Fetches the stored HTML and attaches it to the
// requesting staff member's player connection for chunked streaming by
// tick_chatlog_xfer() on the main thread.
void db_complaint_log(char *opt) {
    char buf[256];
    MYSQL_RES *result;
    MYSQL_ROW row;
    unsigned long *lengths;
    unsigned char *data;
    int staffID, id, cn, nr, len, attached = 0, busy = 0;

    staffID = atoi(opt);
    id = atoi(strchr(opt, ' ') ? strchr(opt, ' ') + 1 : "0");

    sprintf(buf, "select chatlog from complaints where ID=%d", id);

    if (mysql_query_con(&mysql, buf)) {
        elog("Failed to read complaint log %d: Error: %s (%d)", id, mysql_error(&mysql), mysql_errno(&mysql));
        return;
    }
    if (!(result = mysql_store_result_cnt(&mysql))) {
        elog("Failed to store result: Error: %s (%d)", mysql_error(&mysql), mysql_errno(&mysql));
        return;
    }

    if (!(row = mysql_fetch_row(result)) || !row[0] || !(lengths = mysql_fetch_lengths(result)) || !lengths[0]) {
        tell_chat(0, staffID, 1, "Complaint #%d not found (or has no chat log).", id);
        mysql_free_result_cnt(result);
        return;
    }

    len = (int)lengths[0];
    data = xmalloc(len, IM_TEMP);
    if (!data) {
        mysql_free_result_cnt(result);
        return;
    }
    memcpy(data, row[0], len);
    mysql_free_result_cnt(result);

    lock_server();
    for (cn = getfirst_char(); cn; cn = getnext_char(cn)) {
        if (ch[cn].ID == staffID) break;
    }
    if (cn && (nr = ch[cn].player) && nr > 0 && nr < MAXPLAYER && player[nr]) {
        if (player[nr]->xfer_buf) busy = 1;
        else {
            player[nr]->xfer_buf = data;
            player[nr]->xfer_len = len;
            player[nr]->xfer_pos = -1; // START message not sent yet
            player[nr]->xfer_id = id;
            attached = 1;
        }
    }
    unlock_server();

    if (!attached) {
        xfree(data);
        if (busy) tell_chat(0, staffID, 1, "Another chat log transfer is still in progress, please wait.");
        return;
    }

    tell_chat(0, staffID, 1, "Sending chat log for complaint #%d (%d bytes).", id, len);
}
