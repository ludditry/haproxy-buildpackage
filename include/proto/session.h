/*
 * include/proto/session.h
 * This file defines everything related to sessions.
 *
 * Copyright (C) 2000-2010 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _PROTO_SESSION_H
#define _PROTO_SESSION_H

#include <common/config.h>
#include <common/memory.h>
#include <types/session.h>
#include <proto/freq_ctr.h>
#include <proto/stick_table.h>

extern struct pool_head *pool2_session;
extern struct list sessions;

extern struct data_cb sess_conn_cb;

int session_accept(struct listener *l, int cfd, struct sockaddr_storage *addr);

/* perform minimal intializations, report 0 in case of error, 1 if OK. */
int init_session();

/* kill a session and set the termination flags to <why> (one of SN_ERR_*) */
void session_shutdown(struct session *session, int why);

void session_process_counters(struct session *s);
void sess_change_server(struct session *sess, struct server *newsrv);
struct task *process_session(struct task *t);
void default_srv_error(struct session *s, struct stream_interface *si);
int parse_track_counters(char **args, int *arg,
			 int section_type, struct proxy *curpx,
			 struct track_ctr_prm *prm,
			 struct proxy *defpx, char **err);

/* returns the session from a void *owner */
static inline struct session *session_from_task(struct task *t)
{
	return (struct session *)t->context;
}

/* Remove the refcount from the session to the tracked counters, and clear the
 * pointer to ensure this is only performed once. The caller is responsible for
 * ensuring that the pointer is valid first.
 */
static inline void session_store_counters(struct session *s)
{
	void *ptr;
	int i;

	for (i = 0; i < MAX_SESS_STKCTR; i++) {
		if (!s->stkctr[i].entry)
			continue;
		ptr = stktable_data_ptr(s->stkctr[i].table, s->stkctr[i].entry, STKTABLE_DT_CONN_CUR);
		if (ptr)
			stktable_data_cast(ptr, conn_cur)--;
		s->stkctr[i].entry->ref_cnt--;
		stksess_kill_if_expired(s->stkctr[i].table, s->stkctr[i].entry);
		s->stkctr[i].entry = NULL;
	}
}

/* Remove the refcount from the session counters tracked only by the backend if
 * any, and clear the pointer to ensure this is only performed once. The caller
 * is responsible for ensuring that the pointer is valid first.
 */
static inline void session_stop_backend_counters(struct session *s)
{
	void *ptr;
	int i;

	if (likely(!(s->flags & SN_BE_TRACK_ANY)))
		return;

	for (i = 0; i < MAX_SESS_STKCTR; i++) {
		if (!s->stkctr[i].entry)
			continue;

		if (!(s->flags & (SN_BE_TRACK_SC0 << i)))
			continue;

		ptr = stktable_data_ptr(s->stkctr[i].table, s->stkctr[i].entry, STKTABLE_DT_CONN_CUR);
		if (ptr)
			stktable_data_cast(ptr, conn_cur)--;
		s->stkctr[i].entry->ref_cnt--;
		stksess_kill_if_expired(s->stkctr[i].table, s->stkctr[i].entry);
		s->stkctr[i].entry = NULL;
	}
	s->flags &= ~SN_BE_TRACK_ANY;
}

/* Increase total and concurrent connection count for stick entry <ts> of table
 * <t>. The caller is responsible for ensuring that <t> and <ts> are valid
 * pointers, and for calling this only once per connection.
 */
static inline void session_start_counters(struct stktable *t, struct stksess *ts)
{
	void *ptr;

	ptr = stktable_data_ptr(t, ts, STKTABLE_DT_CONN_CUR);
	if (ptr)
		stktable_data_cast(ptr, conn_cur)++;

	ptr = stktable_data_ptr(t, ts, STKTABLE_DT_CONN_CNT);
	if (ptr)
		stktable_data_cast(ptr, conn_cnt)++;

	ptr = stktable_data_ptr(t, ts, STKTABLE_DT_CONN_RATE);
	if (ptr)
		update_freq_ctr_period(&stktable_data_cast(ptr, conn_rate),
				       t->data_arg[STKTABLE_DT_CONN_RATE].u, 1);
	if (tick_isset(t->expire))
		ts->expire = tick_add(now_ms, MS_TO_TICKS(t->expire));
}

/* Enable tracking of session counters as <stkctr> on stksess <ts>. The caller is
 * responsible for ensuring that <t> and <ts> are valid pointers. Some controls
 * are performed to ensure the state can still change.
 */
static inline void session_track_stkctr(struct stkctr *ctr, struct stktable *t, struct stksess *ts)
{
	if (ctr->entry)
		return;

	ts->ref_cnt++;
	ctr->table = t;
	ctr->entry = ts;
	session_start_counters(t, ts);
}

/* Increase the number of cumulated HTTP requests in the tracked counters */
static void inline session_inc_http_req_ctr(struct session *s)
{
	void *ptr;
	int i;

	for (i = 0; i < MAX_SESS_STKCTR; i++) {
		if (!s->stkctr[i].entry)
			continue;

		ptr = stktable_data_ptr(s->stkctr[i].table, s->stkctr[i].entry, STKTABLE_DT_HTTP_REQ_CNT);
		if (ptr)
			stktable_data_cast(ptr, http_req_cnt)++;

		ptr = stktable_data_ptr(s->stkctr[i].table, s->stkctr[i].entry, STKTABLE_DT_HTTP_REQ_RATE);
		if (ptr)
			update_freq_ctr_period(&stktable_data_cast(ptr, http_req_rate),
					       s->stkctr[i].table->data_arg[STKTABLE_DT_HTTP_REQ_RATE].u, 1);
	}
}

/* Increase the number of cumulated HTTP requests in the backend's tracked counters */
static void inline session_inc_be_http_req_ctr(struct session *s)
{
	void *ptr;
	int i;

	if (likely(!(s->flags & SN_BE_TRACK_ANY)))
		return;

	for (i = 0; i < MAX_SESS_STKCTR; i++) {
		if (!s->stkctr[i].entry)
			continue;

		if (!(s->flags & (SN_BE_TRACK_SC0 << i)))
			continue;

		ptr = stktable_data_ptr(s->stkctr[i].table, s->stkctr[i].entry, STKTABLE_DT_HTTP_REQ_CNT);
		if (ptr)
			stktable_data_cast(ptr, http_req_cnt)++;

		ptr = stktable_data_ptr(s->stkctr[i].table, s->stkctr[i].entry, STKTABLE_DT_HTTP_REQ_RATE);
		if (ptr)
			update_freq_ctr_period(&stktable_data_cast(ptr, http_req_rate),
			                       s->stkctr[i].table->data_arg[STKTABLE_DT_HTTP_REQ_RATE].u, 1);
	}
}

/* Increase the number of cumulated failed HTTP requests in the tracked
 * counters. Only 4xx requests should be counted here so that we can
 * distinguish between errors caused by client behaviour and other ones.
 * Note that even 404 are interesting because they're generally caused by
 * vulnerability scans.
 */
static void inline session_inc_http_err_ctr(struct session *s)
{
	void *ptr;
	int i;

	for (i = 0; i < MAX_SESS_STKCTR; i++) {
		if (!s->stkctr[i].entry)
			continue;

		ptr = stktable_data_ptr(s->stkctr[i].table, s->stkctr[i].entry, STKTABLE_DT_HTTP_ERR_CNT);
		if (ptr)
			stktable_data_cast(ptr, http_err_cnt)++;

		ptr = stktable_data_ptr(s->stkctr[i].table, s->stkctr[i].entry, STKTABLE_DT_HTTP_ERR_RATE);
		if (ptr)
			update_freq_ctr_period(&stktable_data_cast(ptr, http_err_rate),
			                       s->stkctr[i].table->data_arg[STKTABLE_DT_HTTP_ERR_RATE].u, 1);
	}
}

static void inline session_add_srv_conn(struct session *sess, struct server *srv)
{
	sess->srv_conn = srv;
	LIST_ADD(&srv->actconns, &sess->by_srv);
}

static void inline session_del_srv_conn(struct session *sess)
{
	if (!sess->srv_conn)
		return;

	sess->srv_conn = NULL;
	LIST_DEL(&sess->by_srv);
}

static void inline session_init_srv_conn(struct session *sess)
{
	sess->srv_conn = NULL;
	LIST_INIT(&sess->by_srv);
}

#endif /* _PROTO_SESSION_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
