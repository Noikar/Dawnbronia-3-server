/*
 * Player complaint system (see complaint.c).
 */

// main thread: build complaint HTML from cn's scrollback and queue the insert
int complaint_new(int cn, int vID, char *vname, char *reason);

// database thread handlers, dispatched from db_thread_sub() in database.c
void db_complaint_add(char *query, char *announce);
void db_complaint_list(char *opt);
void db_complaint_view(char *opt);
void db_complaint_close(char *opt);
void db_complaint_log(char *opt);
