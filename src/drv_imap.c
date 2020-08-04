/*
 * mbsync - mailbox synchronizer
 * Copyright (C) 2000-2002 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2002-2006,2008,2010-2013 Oswald Buddenhagen <ossi@users.sf.net>
 * Copyright (C) 2004 Theodore Y. Ts'o <tytso@mit.edu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, mbsync may be linked with the OpenSSL library,
 * despite that library's more restrictive license.
 */

#include "driver.h"

#include "socket.h"

#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/wait.h>

#ifdef HAVE_LIBSASL
# include <sasl/sasl.h>
# include <sasl/saslutil.h>
#endif

#ifdef HAVE_LIBSSL
# include <openssl/evp.h>
enum { SSL_None, SSL_STARTTLS, SSL_IMAPS };
#endif

typedef struct imap_server_conf {
	struct imap_server_conf *next;
	char *name;
	server_conf_t sconf;
	char *user;
	char *pass;
	char *pass_cmd;
	int max_in_progress;
	int cap_mask;
	string_list_t *auth_mechs;
#ifdef HAVE_LIBSSL
	char ssl_type;
#endif
	char failed;
} imap_server_conf_t;

typedef struct {
	store_conf_t gen;
	imap_server_conf_t *server;
	char delimiter;
	char use_namespace;
} imap_store_conf_t;

typedef struct {
	message_t gen;
/*	int seq; will be needed when expunges are tracked */
} imap_message_t;

#define NIL	(void*)0x1
#define LIST	(void*)0x2

typedef struct _list {
	struct _list *next, *child;
	char *val;
	int len;
} list_t;

#define MAX_LIST_DEPTH 5

typedef struct imap_store imap_store_t;

typedef struct {
	list_t *head, **stack[MAX_LIST_DEPTH];
	int (*callback)( imap_store_t *ctx, list_t *list, char *cmd );
	int level, need_bytes;
} parse_list_state_t;

typedef struct imap_cmd imap_cmd_t;

struct imap_store {
	store_t gen;
	const char *label; /* foreign */
	const char *prefix;
	const char *name;
	int ref_count;
	uint opts;
	enum { SST_BAD, SST_HALF, SST_GOOD } state;
	/* trash folder's existence is not confirmed yet */
	enum { TrashUnknown, TrashChecking, TrashKnown } trashnc;
	uint got_namespace:1;
	char delimiter[2]; /* hierarchy delimiter */
	list_t *ns_personal, *ns_other, *ns_shared; /* NAMESPACE info */
	string_list_t *boxes; // _list results
	char listed; // was _list already run with these flags?
	// note that the message counts do _not_ reflect stats from msgs,
	// but mailbox totals. also, don't trust them beyond the initial load.
	int total_msgs, recent_msgs;
	uint uidvalidity, uidnext;
	message_t *msgs;
	message_t **msgapp; /* FETCH results */
	uint caps; /* CAPABILITY results */
	string_list_t *auth_mechs;
	parse_list_state_t parse_list_sts;
	/* command queue */
	int nexttag, num_in_progress;
	imap_cmd_t *pending, **pending_append;
	imap_cmd_t *in_progress, **in_progress_append;
	int buffer_mem; /* memory currently occupied by buffers in the queue */

	/* Used during sequential operations like connect */
	enum { GreetingPending = 0, GreetingBad, GreetingOk, GreetingPreauth } greeting;
	int expectBYE; /* LOGOUT is in progress */
	int expectEOF; /* received LOGOUT's OK or unsolicited BYE */
	int canceling; /* imap_cancel() is in progress */
	union {
		void (*imap_open)( int sts, void *aux );
		void (*imap_cancel)( void *aux );
	} callbacks;
	void *callback_aux;
#ifdef HAVE_LIBSASL
	sasl_conn_t *sasl;
	int sasl_cont;
#endif

	void (*bad_callback)( void *aux );
	void *bad_callback_aux;

	conn_t conn; /* this is BIG, so put it last */
};

struct imap_cmd {
	struct imap_cmd *next;
	char *cmd;
	int tag;

	struct {
		/* Will be called on each continuation request until it resets this pointer.
		 * Needs to invoke bad_callback and return -1 on error, otherwise return 0. */
		int (*cont)( imap_store_t *ctx, imap_cmd_t *cmd, const char *prompt );
		void (*done)( imap_store_t *ctx, imap_cmd_t *cmd, int response );
		char *data;
		int data_len;
		uint uid; /* to identify fetch responses */
		char high_prio; /* if command is queued, put it at the front of the queue. */
		char to_trash; /* we are storing to trash, not current. */
		char create; /* create the mailbox if we get an error which suggests so. */
		char failok; /* Don't complain about NO response. */
		char lastuid; /* querying the last UID in the mailbox. */
	} param;
};

typedef struct {
	imap_cmd_t gen;
	void (*callback)( int sts, void *aux );
	void *callback_aux;
} imap_cmd_simple_t;

typedef struct {
	imap_cmd_simple_t gen;
	msg_data_t *msg_data;
} imap_cmd_fetch_msg_t;

typedef struct {
	imap_cmd_t gen;
	void (*callback)( int sts, uint uid, void *aux );
	void *callback_aux;
	uint out_uid;
} imap_cmd_out_uid_t;

typedef struct {
	imap_cmd_t gen;
	void (*callback)( int sts, message_t *msgs, void *aux );
	void *callback_aux;
	message_t **out_msgs;
	uint uid;
} imap_cmd_find_new_t;

typedef struct {
	int ref_count;
	int ret_val;
} imap_cmd_refcounted_state_t;

typedef struct {
	imap_cmd_t gen;
	imap_cmd_refcounted_state_t *state;
} imap_cmd_refcounted_t;

#define CAP(cap) (ctx->caps & (1 << (cap)))

enum CAPABILITY {
	NOLOGIN = 0,
#ifdef HAVE_LIBSASL
	SASLIR,
#endif
#ifdef HAVE_LIBSSL
	STARTTLS,
#endif
	UIDPLUS,
	LITERALPLUS,
	MOVE,
	NAMESPACE,
	COMPRESS_DEFLATE
};

static const char *cap_list[] = {
	"LOGINDISABLED",
#ifdef HAVE_LIBSASL
	"SASL-IR",
#endif
#ifdef HAVE_LIBSSL
	"STARTTLS",
#endif
	"UIDPLUS",
	"LITERAL+",
	"MOVE",
	"NAMESPACE",
	"COMPRESS=DEFLATE"
};

#define RESP_OK       0
#define RESP_NO       1
#define RESP_CANCEL   2

static INLINE void imap_ref( imap_store_t *ctx ) { ++ctx->ref_count; }
static int imap_deref( imap_store_t *ctx );

static void imap_invoke_bad_callback( imap_store_t *ctx );

static const char *Flags[] = {
	"Draft",
	"Flagged",
	"Answered",
	"Seen",
	"Deleted",
};

static imap_cmd_t *
new_imap_cmd( int size )
{
	imap_cmd_t *cmd = nfmalloc( size );
	memset( &cmd->param, 0, sizeof(cmd->param) );
	return cmd;
}

#define INIT_IMAP_CMD(type, cmdp, cb, aux) \
	cmdp = (type *)new_imap_cmd( sizeof(*cmdp) ); \
	cmdp->callback = cb; \
	cmdp->callback_aux = aux;

#define INIT_IMAP_CMD_X(type, cmdp, cb, aux) \
	cmdp = (type *)new_imap_cmd( sizeof(*cmdp) ); \
	cmdp->gen.callback = cb; \
	cmdp->gen.callback_aux = aux;

static void
done_imap_cmd( imap_store_t *ctx, imap_cmd_t *cmd, int response )
{
	cmd->param.done( ctx, cmd, response );
	if (cmd->param.data) {
		free( cmd->param.data );
		ctx->buffer_mem -= cmd->param.data_len;
	}
	free( cmd->cmd );
	free( cmd );
}

static void
send_imap_cmd( imap_store_t *ctx, imap_cmd_t *cmd )
{
	int bufl, litplus, iovcnt = 1;
	const char *buffmt;
	conn_iovec_t iov[3];
	char buf[4096];

	cmd->tag = ++ctx->nexttag;
	if (!cmd->param.data) {
		buffmt = "%d %s\r\n";
		litplus = 0;
	} else if ((cmd->param.to_trash && ctx->trashnc == TrashUnknown) || !CAP(LITERALPLUS) || cmd->param.data_len >= 100*1024) {
		buffmt = "%d %s{%d}\r\n";
		litplus = 0;
	} else {
		buffmt = "%d %s{%d+}\r\n";
		litplus = 1;
	}
	bufl = nfsnprintf( buf, sizeof(buf), buffmt,
	                   cmd->tag, cmd->cmd, cmd->param.data_len );
	if (DFlags & DEBUG_NET) {
		if (ctx->num_in_progress)
			printf( "(%d in progress) ", ctx->num_in_progress );
		if (starts_with( cmd->cmd, -1, "LOGIN", 5 ))
			printf( "%s>>> %d LOGIN <user> <pass>\n", ctx->label, cmd->tag );
		else if (starts_with( cmd->cmd, -1, "AUTHENTICATE PLAIN", 18 ))
			printf( "%s>>> %d AUTHENTICATE PLAIN <authdata>\n", ctx->label, cmd->tag );
		else
			printf( "%s>>> %s", ctx->label, buf );
		fflush( stdout );
	}
	iov[0].buf = buf;
	iov[0].len = bufl;
	iov[0].takeOwn = KeepOwn;
	if (litplus) {
		if (DFlags & DEBUG_NET_ALL) {
			printf( "%s>>>>>>>>>\n", ctx->label );
			fwrite( cmd->param.data, cmd->param.data_len, 1, stdout );
			printf( "%s>>>>>>>>>\n", ctx->label );
			fflush( stdout );
		}
		iov[1].buf = cmd->param.data;
		iov[1].len = cmd->param.data_len;
		iov[1].takeOwn = GiveOwn;
		cmd->param.data = 0;
		ctx->buffer_mem -= cmd->param.data_len;
		iov[2].buf = "\r\n";
		iov[2].len = 2;
		iov[2].takeOwn = KeepOwn;
		iovcnt = 3;
	}
	socket_write( &ctx->conn, iov, iovcnt );
	if (cmd->param.to_trash && ctx->trashnc == TrashUnknown)
		ctx->trashnc = TrashChecking;
	cmd->next = 0;
	*ctx->in_progress_append = cmd;
	ctx->in_progress_append = &cmd->next;
	ctx->num_in_progress++;
	socket_expect_read( &ctx->conn, 1 );
}

static int
cmd_sendable( imap_store_t *ctx, imap_cmd_t *cmd )
{
	if (ctx->conn.write_buf) {
		/* Don't build up a long queue in the socket, so we can
		 * control when the commands are actually sent.
		 * This allows reliable cancelation of pending commands,
		 * injecting commands in front of other pending commands,
		 * and keeping num_in_progress accurate. */
		return 0;
	}
	if (ctx->in_progress) {
		/* If the last command in flight ... */
		imap_cmd_t *cmdp = (imap_cmd_t *)((char *)ctx->in_progress_append -
		                                  offsetof(imap_cmd_t, next));
		if (cmdp->param.cont || cmdp->param.data) {
			/* ... is expected to trigger a continuation request, we need to
			 * wait for that round-trip before sending the next command. */
			return 0;
		}
	}
	if (cmd->param.to_trash && ctx->trashnc == TrashChecking) {
		/* Don't build a queue of MOVE/COPY/APPEND commands that may all fail. */
		return 0;
	}
	if (ctx->num_in_progress >= ((imap_store_conf_t *)ctx->gen.conf)->server->max_in_progress) {
		/* Too many commands in flight. */
		return 0;
	}
	return 1;
}

static void
flush_imap_cmds( imap_store_t *ctx )
{
	imap_cmd_t *cmd;

	if ((cmd = ctx->pending) && cmd_sendable( ctx, cmd )) {
		if (!(ctx->pending = cmd->next))
			ctx->pending_append = &ctx->pending;
		send_imap_cmd( ctx, cmd );
	}
}

static void
cancel_pending_imap_cmds( imap_store_t *ctx )
{
	imap_cmd_t *cmd;

	while ((cmd = ctx->pending)) {
		if (!(ctx->pending = cmd->next))
			ctx->pending_append = &ctx->pending;
		done_imap_cmd( ctx, cmd, RESP_CANCEL );
	}
}

static void
cancel_sent_imap_cmds( imap_store_t *ctx )
{
	imap_cmd_t *cmd;

	socket_expect_read( &ctx->conn, 0 );
	while ((cmd = ctx->in_progress)) {
		ctx->in_progress = cmd->next;
		/* don't update num_in_progress and in_progress_append - store is dead */
		done_imap_cmd( ctx, cmd, RESP_CANCEL );
	}
}

static void
submit_imap_cmd( imap_store_t *ctx, imap_cmd_t *cmd )
{
	assert( ctx );
	assert( ctx->bad_callback );
	assert( cmd );
	assert( cmd->param.done );

	if ((ctx->pending && !cmd->param.high_prio) || !cmd_sendable( ctx, cmd )) {
		if (ctx->pending && cmd->param.high_prio) {
			cmd->next = ctx->pending;
			ctx->pending = cmd;
		} else {
			cmd->next = 0;
			*ctx->pending_append = cmd;
			ctx->pending_append = &cmd->next;
		}
	} else {
		send_imap_cmd( ctx, cmd );
	}
}

/* Minimal printf() replacement that supports an %\s format sequence to print backslash-escaped
 * string literals. Note that this does not automatically add quotes around the printed string,
 * so it is possible to concatenate multiple segments. */
static char *
imap_vprintf( const char *fmt, va_list ap )
{
	const char *s;
	char *d, *ed;
	int maxlen;
	char c;
	char buf[4096];

	d = buf;
	ed = d + sizeof(buf);
	s = fmt;
	for (;;) {
		c = *fmt;
		if (!c || c == '%') {
			int l = fmt - s;
			if (d + l > ed)
				oob();
			memcpy( d, s, l );
			d += l;
			if (!c)
				return nfstrndup( buf, d - buf );
			maxlen = INT_MAX;
			c = *++fmt;
			if (c == '\\') {
				c = *++fmt;
				if (c != 's') {
					fputs( "Fatal: unsupported escaped format specifier. Please report a bug.\n", stderr );
					abort();
				}
				s = va_arg( ap, const char * );
				while ((c = *s++)) {
					if (d + 2 > ed)
						oob();
					if (c == '\\' || c == '"')
						*d++ = '\\';
					*d++ = c;
				}
			} else { /* \\ cannot be combined with anything else. */
				if (c == '.') {
					c = *++fmt;
					if (c != '*') {
						fputs( "Fatal: unsupported string length specification. Please report a bug.\n", stderr );
						abort();
					}
					maxlen = va_arg( ap , int );
					c = *++fmt;
				}
				if (c == 'c') {
					if (d + 1 > ed)
						oob();
					*d++ = (char)va_arg( ap , int );
				} else if (c == 's') {
					s = va_arg( ap, const char * );
					l = strnlen( s, maxlen );
					if (d + l > ed)
						oob();
					memcpy( d, s, l );
					d += l;
				} else if (c == 'd') {
					d += nfsnprintf( d, ed - d, "%d", va_arg( ap , int ) );
				} else if (c == 'u') {
					d += nfsnprintf( d, ed - d, "%u", va_arg( ap , uint ) );
				} else {
					fputs( "Fatal: unsupported format specifier. Please report a bug.\n", stderr );
					abort();
				}
			}
			s = ++fmt;
		} else {
			fmt++;
		}
	}
}

static void
imap_exec( imap_store_t *ctx, imap_cmd_t *cmdp,
           void (*done)( imap_store_t *ctx, imap_cmd_t *cmd, int response ),
           const char *fmt, ... )
{
	va_list ap;

	if (!cmdp)
		cmdp = new_imap_cmd( sizeof(*cmdp) );
	cmdp->param.done = done;
	va_start( ap, fmt );
	cmdp->cmd = imap_vprintf( fmt, ap );
	va_end( ap );
	submit_imap_cmd( ctx, cmdp );
}

static void
transform_box_response( int *response )
{
	switch (*response) {
	case RESP_CANCEL: *response = DRV_CANCELED; break;
	case RESP_NO: *response = DRV_BOX_BAD; break;
	default: *response = DRV_OK; break;
	}
}

static void
imap_done_simple_box( imap_store_t *ctx ATTR_UNUSED,
                      imap_cmd_t *cmd, int response )
{
	imap_cmd_simple_t *cmdp = (imap_cmd_simple_t *)cmd;

	transform_box_response( &response );
	cmdp->callback( response, cmdp->callback_aux );
}

static void
transform_msg_response( int *response )
{
	switch (*response) {
	case RESP_CANCEL: *response = DRV_CANCELED; break;
	case RESP_NO: *response = DRV_MSG_BAD; break;
	default: *response = DRV_OK; break;
	}
}

static void
imap_done_simple_msg( imap_store_t *ctx ATTR_UNUSED,
                      imap_cmd_t *cmd, int response )
{
	imap_cmd_simple_t *cmdp = (imap_cmd_simple_t *)cmd;

	transform_msg_response( &response );
	cmdp->callback( response, cmdp->callback_aux );
}

static imap_cmd_refcounted_state_t *
imap_refcounted_new_state( int sz )
{
	imap_cmd_refcounted_state_t *sts = nfmalloc( sz );
	sts->ref_count = 1; /* so forced sync does not cause an early exit */
	sts->ret_val = DRV_OK;
	return sts;
}

#define INIT_REFCOUNTED_STATE(type, sts, cb, aux) \
	type *sts = (type *)imap_refcounted_new_state( sizeof(type) ); \
	sts->callback = cb; \
	sts->callback_aux = aux;

static imap_cmd_t *
imap_refcounted_new_cmd( imap_cmd_refcounted_state_t *sts )
{
	imap_cmd_refcounted_t *cmd = (imap_cmd_refcounted_t *)new_imap_cmd( sizeof(*cmd) );
	cmd->state = sts;
	sts->ref_count++;
	return &cmd->gen;
}

#define DONE_REFCOUNTED_STATE(sts) \
	if (!--sts->gen.ref_count) { \
		sts->callback( sts->gen.ret_val, sts->callback_aux ); \
		free( sts ); \
	}

#define DONE_REFCOUNTED_STATE_ARGS(sts, finalize, ...) \
	if (!--sts->gen.ref_count) { \
		finalize \
		sts->callback( sts->gen.ret_val, __VA_ARGS__, sts->callback_aux ); \
		free( sts ); \
	}

static void
transform_refcounted_box_response( imap_cmd_refcounted_state_t *sts, int response )
{
	switch (response) {
	case RESP_CANCEL:
		sts->ret_val = DRV_CANCELED;
		break;
	case RESP_NO:
		if (sts->ret_val == DRV_OK) /* Don't override cancelation. */
			sts->ret_val = DRV_BOX_BAD;
		break;
	}
}

static void
transform_refcounted_msg_response( imap_cmd_refcounted_state_t *sts, int response )
{
	switch (response) {
	case RESP_CANCEL:
		sts->ret_val = DRV_CANCELED;
		break;
	case RESP_NO:
		if (sts->ret_val == DRV_OK) /* Don't override cancelation. */
			sts->ret_val = DRV_MSG_BAD;
		break;
	}
}

static const char *
imap_strchr( const char *s, char tc )
{
	for (;; s++) {
		char c = *s;
		if (c == '\\')
			c = *++s;
		if (!c)
			return 0;
		if (c == tc)
			return s;
	}
}

static char *
next_arg( char **ps )
{
	char *ret, *s, *d;
	char c;

	assert( ps );
	s = *ps;
	if (!s)
		return 0;
	while (isspace( (uchar)*s ))
		s++;
	if (!*s) {
		*ps = 0;
		return 0;
	}
	if (*s == '"') {
		s++;
		ret = d = s;
		while ((c = *s++) != '"') {
			if (c == '\\')
				c = *s++;
			if (!c) {
				*ps = 0;
				return 0;
			}
			*d++ = c;
		}
		*d = 0;
	} else {
		ret = s;
		while ((c = *s)) {
			if (isspace( (uchar)c )) {
				*s++ = 0;
				break;
			}
			s++;
		}
	}
	if (!*s)
		s = 0;

	*ps = s;
	return ret;
}

static int
is_opt_atom( list_t *list )
{
	return list && list->val && list->val != LIST;
}

static int
is_atom( list_t *list )
{
	return list && list->val && list->val != NIL && list->val != LIST;
}

static int
is_list( list_t *list )
{
	return list && list->val == LIST;
}

static void
free_list( list_t *list )
{
	list_t *tmp;

	for (; list; list = tmp) {
		tmp = list->next;
		if (is_list( list ))
			free_list( list->child );
		else if (is_atom( list ))
			free( list->val );
		free( list );
	}
}

enum {
	LIST_OK,
	LIST_PARTIAL,
	LIST_BAD
};

static int
parse_imap_list( imap_store_t *ctx, char **sp, parse_list_state_t *sts )
{
	list_t *cur, **curp;
	char *s = *sp, *d, *p;
	int n, bytes;
	char c;

	assert( sts );
	assert( sts->level > 0 );
	curp = sts->stack[--sts->level];
	bytes = sts->need_bytes;
	if (bytes >= 0) {
		sts->need_bytes = -1;
		if (!bytes)
			goto getline;
		cur = (list_t *)((char *)curp - offsetof(list_t, next));
		s = cur->val + cur->len - bytes;
		goto getbytes;
	}

	if (!s)
		return LIST_BAD;
	for (;;) {
		while (isspace( (uchar)*s ))
			s++;
		if (sts->level && *s == ')') {
			s++;
			curp = sts->stack[--sts->level];
			goto next;
		}
		*curp = cur = nfmalloc( sizeof(*cur) );
		cur->val = 0; /* for clean bail */
		curp = &cur->next;
		*curp = 0; /* ditto */
		if (*s == '(') {
			/* sublist */
			if (sts->level == MAX_LIST_DEPTH)
				goto bail;
			s++;
			cur->val = LIST;
			sts->stack[sts->level++] = curp;
			curp = &cur->child;
			*curp = 0; /* for clean bail */
			goto next2;
		} else if (ctx && *s == '{') {
			/* literal */
			bytes = cur->len = strtol( s + 1, &s, 10 );
			if (*s != '}' || *++s)
				goto bail;

			s = cur->val = nfmalloc( cur->len + 1 );
			s[cur->len] = 0;

		  getbytes:
			n = socket_read( &ctx->conn, s, bytes );
			if (n < 0) {
			  badeof:
				error( "IMAP error: unexpected EOF from %s\n", ctx->conn.name );
				goto bail;
			}
			bytes -= n;
			if (bytes > 0)
				goto postpone;

			if (DFlags & DEBUG_NET_ALL) {
				printf( "%s=========\n", ctx->label );
				fwrite( cur->val, cur->len, 1, stdout );
				printf( "%s=========\n", ctx->label );
				fflush( stdout );
			}

		  getline:
			if (!(s = socket_read_line( &ctx->conn )))
				goto postpone;
			if (s == (void *)~0)
				goto badeof;
			if (DFlags & DEBUG_NET) {
				printf( "%s%s\n", ctx->label, s );
				fflush( stdout );
			}
		} else if (*s == '"') {
			/* quoted string */
			s++;
			p = d = s;
			while ((c = *s++) != '"') {
				if (c == '\\')
					c = *s++;
				if (!c)
					goto bail;
				*d++ = c;
			}
			cur->len = d - p;
			cur->val = nfstrndup( p, cur->len );
		} else {
			/* atom */
			p = s;
			for (; *s && !isspace( (uchar)*s ); s++)
				if (sts->level && *s == ')')
					break;
			cur->len = s - p;
			if (equals( p, cur->len, "NIL", 3 ))
				cur->val = NIL;
			else
				cur->val = nfstrndup( p, cur->len );
		}

	  next:
		if (!sts->level)
			break;
	  next2:
		if (!*s)
			goto bail;
	}
	*sp = s;
	return LIST_OK;

  postpone:
	if (sts->level < MAX_LIST_DEPTH) {
		sts->stack[sts->level++] = curp;
		sts->need_bytes = bytes;
		return LIST_PARTIAL;
	}
  bail:
	free_list( sts->head );
	return LIST_BAD;
}

static void
parse_list_init( parse_list_state_t *sts )
{
	sts->need_bytes = -1;
	sts->level = 1;
	sts->head = 0;
	sts->stack[0] = &sts->head;
}

static int
parse_list_continue( imap_store_t *ctx, char *s )
{
	list_t *list;
	int resp;
	if ((resp = parse_imap_list( ctx, &s, &ctx->parse_list_sts )) != LIST_PARTIAL) {
		list = (resp == LIST_BAD) ? 0 : ctx->parse_list_sts.head;
		ctx->parse_list_sts.head = 0;
		resp = ctx->parse_list_sts.callback( ctx, list, s );
	}
	return resp;
}

static int
parse_list( imap_store_t *ctx, char *s, int (*cb)( imap_store_t *ctx, list_t *list, char *s ) )
{
	parse_list_init( &ctx->parse_list_sts );
	ctx->parse_list_sts.callback = cb;
	return parse_list_continue( ctx, s );
}

static int parse_namespace_rsp_p2( imap_store_t *, list_t *, char * );
static int parse_namespace_rsp_p3( imap_store_t *, list_t *, char * );

static int
parse_namespace_check( list_t *list )
{
	if (!list)
		goto bad;
	if (list->val == NIL)
		return 0;
	if (list->val != LIST)
		goto bad;
	for (list = list->child; list; list = list->next) {
		if (list->val != LIST)
			goto bad;
		if (!is_atom( list->child ))
			goto bad;
		if (!is_opt_atom( list->child->next ))
			goto bad;
		/* Namespace response extensions may follow here; we don't care. */
	}
	return 0;
  bad:
	error( "IMAP error: malformed NAMESPACE response\n" );
	return -1;
}

static int
parse_namespace_rsp( imap_store_t *ctx, list_t *list, char *s )
{
	if (parse_namespace_check( (ctx->ns_personal = list) ))
		return LIST_BAD;
	return parse_list( ctx, s, parse_namespace_rsp_p2 );
}

static int
parse_namespace_rsp_p2( imap_store_t *ctx, list_t *list, char *s )
{
	if (parse_namespace_check( (ctx->ns_other = list) ))
		return LIST_BAD;
	return parse_list( ctx, s, parse_namespace_rsp_p3 );
}

static int
parse_namespace_rsp_p3( imap_store_t *ctx, list_t *list, char *s ATTR_UNUSED )
{
	if (parse_namespace_check( (ctx->ns_shared = list) ))
		return LIST_BAD;
	return LIST_OK;
}

static time_t
parse_date( const char *str )
{
	char *end;
	time_t date;
	int hours, mins;
	struct tm datetime;

	memset( &datetime, 0, sizeof(datetime) );
	if (!(end = strptime( str, "%e-%b-%Y %H:%M:%S ", &datetime )))
		return -1;
	if ((date = timegm( &datetime )) == -1)
		return -1;
	if (sscanf( end, "%3d%2d", &hours, &mins ) != 2)
		return -1;
	return date - (hours * 60 + mins) * 60;
}

static int
parse_fetch_rsp( imap_store_t *ctx, list_t *list, char *s ATTR_UNUSED )
{
	list_t *tmp, *flags;
	char *body = 0, *tuid = 0, *msgid = 0, *ep;
	imap_message_t *cur;
	msg_data_t *msgdata;
	imap_cmd_t *cmdp;
	int mask = 0, status = 0, size = 0;
	uint i, uid = 0;
	time_t date = 0;

	if (!is_list( list )) {
		error( "IMAP error: bogus FETCH response\n" );
		free_list( list );
		return LIST_BAD;
	}

	for (tmp = list->child; tmp; tmp = tmp->next) {
		if (is_atom( tmp )) {
			if (!strcmp( "UID", tmp->val )) {
				tmp = tmp->next;
				if (!is_atom( tmp ) || (uid = strtoul( tmp->val, &ep, 10 ), *ep))
					error( "IMAP error: unable to parse UID\n" );
			} else if (!strcmp( "FLAGS", tmp->val )) {
				tmp = tmp->next;
				if (is_list( tmp )) {
					for (flags = tmp->child; flags; flags = flags->next) {
						if (is_atom( flags )) {
							if (flags->val[0] == '\\') { /* ignore user-defined flags for now */
								if (!strcmp( "Recent", flags->val + 1)) {
									status |= M_RECENT;
									goto flagok;
								}
								for (i = 0; i < as(Flags); i++)
									if (!strcmp( Flags[i], flags->val + 1 )) {
										mask |= 1 << i;
										goto flagok;
									}
								if (flags->val[1] == 'X' && flags->val[2] == '-')
									goto flagok; /* ignore system flag extensions */
								error( "IMAP warning: unknown system flag %s\n", flags->val );
							}
						  flagok: ;
						} else
							error( "IMAP error: unable to parse FLAGS list\n" );
					}
					status |= M_FLAGS;
				} else
					error( "IMAP error: unable to parse FLAGS\n" );
			} else if (!strcmp( "INTERNALDATE", tmp->val )) {
				tmp = tmp->next;
				if (is_atom( tmp )) {
					if ((date = parse_date( tmp->val )) == -1)
						error( "IMAP error: unable to parse INTERNALDATE format\n" );
				} else
					error( "IMAP error: unable to parse INTERNALDATE\n" );
			} else if (!strcmp( "RFC822.SIZE", tmp->val )) {
				tmp = tmp->next;
				if (!is_atom( tmp ) || (size = strtoul( tmp->val, &ep, 10 ), *ep))
					error( "IMAP error: unable to parse RFC822.SIZE\n" );
			} else if (!strcmp( "BODY[]", tmp->val )) {
				tmp = tmp->next;
				if (is_atom( tmp )) {
					body = tmp->val;
					tmp->val = 0;       /* don't free together with list */
					size = tmp->len;
				} else
					error( "IMAP error: unable to parse BODY[]\n" );
			} else if (!strcmp( "BODY[HEADER.FIELDS", tmp->val )) {
				tmp = tmp->next;
				if (is_list( tmp )) {
					tmp = tmp->next;
					if (!is_atom( tmp ) || strcmp( tmp->val, "]" ))
						goto bfail;
					tmp = tmp->next;
					if (!is_atom( tmp ))
						goto bfail;
					int off, in_msgid = 0;
					for (char *val = tmp->val, *end; (end = strchr( val, '\n' )); val = end + 1) {
						int len = (int)(end - val);
						if (len && end[-1] == '\r')
							len--;
						if (!len)
							break;
						if (starts_with_upper( val, len, "X-TUID: ", 8 )) {
							if (len < 8 + TUIDL) {
								error( "IMAP error: malformed X-TUID header (UID %u)\n", uid );
								continue;
							}
							tuid = val + 8;
							in_msgid = 0;
							continue;
						}
						if (starts_with_upper( val, len, "MESSAGE-ID:", 11 )) {
							off = 11;
						} else if (in_msgid) {
							if (!isspace( val[0] )) {
								in_msgid = 0;
								continue;
							}
							off = 1;
						} else {
							continue;
						}
						while (off < len && isspace( val[off] ))
							off++;
						if (off == len) {
							in_msgid = 1;
							continue;
						}
						msgid = nfstrndup( val + off, len - off );
						in_msgid = 0;
					}
				} else {
				  bfail:
					error( "IMAP error: unable to parse BODY[HEADER.FIELDS ...]\n" );
				}
			}
		}
	}

	if (!uid) {
		assert( !body && !tuid && !msgid );
		// Ignore async flag updates for now.
	} else if ((cmdp = ctx->in_progress) && cmdp->param.lastuid) {
		assert( !body && !tuid && !msgid );
		// Workaround for server not sending UIDNEXT and/or APPENDUID.
		ctx->uidnext = uid + 1;
	} else if (body) {
		assert( !tuid && !msgid );
		for (cmdp = ctx->in_progress; cmdp; cmdp = cmdp->next)
			if (cmdp->param.uid == uid)
				goto gotuid;
		error( "IMAP error: unexpected FETCH response (UID %u)\n", uid );
		free_list( list );
		return LIST_BAD;
	  gotuid:
		msgdata = ((imap_cmd_fetch_msg_t *)cmdp)->msg_data;
		msgdata->data = body;
		msgdata->len = size;
		msgdata->date = date;
		if (status & M_FLAGS)
			msgdata->flags = mask;
	} else {
		cur = nfcalloc( sizeof(*cur) );
		*ctx->msgapp = &cur->gen;
		ctx->msgapp = &cur->gen.next;
		cur->gen.next = 0;
		cur->gen.uid = uid;
		cur->gen.flags = mask;
		cur->gen.status = status;
		cur->gen.size = size;
		cur->gen.srec = 0;
		cur->gen.msgid = msgid;
		if (tuid)
			memcpy( cur->gen.tuid, tuid, TUIDL );
		else
			cur->gen.tuid[0] = 0;
	}

	free_list( list );
	return LIST_OK;
}

static void
parse_capability( imap_store_t *ctx, char *cmd )
{
	char *arg;
	uint i;

	free_string_list( ctx->auth_mechs );
	ctx->auth_mechs = 0;
	ctx->caps = 0x80000000;
	while ((arg = next_arg( &cmd ))) {
		if (starts_with( arg, -1, "AUTH=", 5 )) {
			add_string_list( &ctx->auth_mechs, arg + 5 );
		} else {
			for (i = 0; i < as(cap_list); i++)
				if (!strcmp( cap_list[i], arg ))
					ctx->caps |= 1 << i;
		}
	}
	ctx->caps &= ~((imap_store_conf_t *)ctx->gen.conf)->server->cap_mask;
	if (!CAP(NOLOGIN))
		add_string_list( &ctx->auth_mechs, "LOGIN" );
}

static int
parse_response_code( imap_store_t *ctx, imap_cmd_t *cmd, char *s )
{
	char *arg, *earg, *p;

	if (!s || *s != '[')
		return RESP_OK;		/* no response code */
	s++;
	if (!(p = strchr( s, ']' ))) {
	  bad_resp:
		error( "IMAP error: malformed response code\n" );
		return RESP_CANCEL;
	}
	*p++ = 0;
	if (!(arg = next_arg( &s )))
		goto bad_resp;
	if (!strcmp( "UIDVALIDITY", arg )) {
		if (!(arg = next_arg( &s )) ||
		    (ctx->uidvalidity = strtoul( arg, &earg, 10 ), *earg))
		{
			error( "IMAP error: malformed UIDVALIDITY status\n" );
			return RESP_CANCEL;
		}
	} else if (!strcmp( "UIDNEXT", arg )) {
		if (!(arg = next_arg( &s )) ||
		    (ctx->uidnext = strtoul( arg, &earg, 10 ), *earg))
		{
			error( "IMAP error: malformed UIDNEXT status\n" );
			return RESP_CANCEL;
		}
	} else if (!strcmp( "CAPABILITY", arg )) {
		parse_capability( ctx, s );
	} else if (!strcmp( "ALERT", arg )) {
		/* RFC2060 says that these messages MUST be displayed
		 * to the user
		 */
		for (; isspace( (uchar)*p ); p++);
		error( "*** IMAP ALERT *** %s\n", p );
	} else if (cmd && !strcmp( "APPENDUID", arg )) {
		if (!(arg = next_arg( &s )) ||
		    (ctx->uidvalidity = strtoul( arg, &earg, 10 ), *earg) ||
		    !(arg = next_arg( &s )) ||
		    (((imap_cmd_out_uid_t *)cmd)->out_uid = strtoul( arg, &earg, 10 ), *earg))
		{
			error( "IMAP error: malformed APPENDUID status\n" );
			return RESP_CANCEL;
		}
	}
	return RESP_OK;
}

static int parse_list_rsp_p1( imap_store_t *, list_t *, char * );
static int parse_list_rsp_p2( imap_store_t *, list_t *, char * );

static int
parse_list_rsp( imap_store_t *ctx, list_t *list, char *cmd )
{
	list_t *lp;

	if (!is_list( list )) {
		free_list( list );
		error( "IMAP error: malformed LIST response\n" );
		return LIST_BAD;
	}
	for (lp = list->child; lp; lp = lp->next)
		if (is_atom( lp ) && !strcasecmp( lp->val, "\\NoSelect" )) {
			free_list( list );
			return LIST_OK;
		}
	free_list( list );
	return parse_list( ctx, cmd, parse_list_rsp_p1 );
}

static int
parse_list_rsp_p1( imap_store_t *ctx, list_t *list, char *cmd ATTR_UNUSED )
{
	if (!is_opt_atom( list )) {
		error( "IMAP error: malformed LIST response\n" );
		free_list( list );
		return LIST_BAD;
	}
	if (!ctx->delimiter[0] && is_atom( list ))
		ctx->delimiter[0] = list->val[0];
	return parse_list( ctx, cmd, parse_list_rsp_p2 );
}

// Use this to check whether a full path refers to the actual IMAP INBOX.
static int
is_inbox( imap_store_t *ctx, const char *arg, int argl )
{
	if (!starts_with_upper( arg, argl, "INBOX", 5 ))
		return 0;
	if (arg[5] && arg[5] != ctx->delimiter[0])
		return 0;
	return 1;
}

// Use this to check whether a path fragment collides with the canonical INBOX.
static int
is_INBOX( imap_store_t *ctx, const char *arg, int argl )
{
	if (!starts_with( arg, argl, "INBOX", 5 ))
		return 0;
	if (arg[5] && arg[5] != ctx->delimiter[0])
		return 0;
	return 1;
}

static int
parse_list_rsp_p2( imap_store_t *ctx, list_t *list, char *cmd ATTR_UNUSED )
{
	string_list_t *narg;
	char *arg;
	int argl, l;

	if (!is_atom( list )) {
		error( "IMAP error: malformed LIST response\n" );
		free_list( list );
		return LIST_BAD;
	}
	arg = list->val;
	argl = list->len;
	if (is_inbox( ctx, arg, argl )) {
		// The server might be weird and have a non-uppercase INBOX. It
		// may legitimately do so, but we need the canonical spelling.
		memcpy( arg, "INBOX", 5 );
	} else if ((l = strlen( ctx->prefix ))) {
		if (!starts_with( arg, argl, ctx->prefix, l ))
			goto skip;
		arg += l;
		argl -= l;
		// A folder named "INBOX" would be indistinguishable from the
		// actual INBOX after prefix stripping, so drop it. This applies
		// only to the fully uppercased spelling, as our canonical box
		// names are case-sensitive (unlike IMAP's INBOX).
		if (is_INBOX( ctx, arg, argl )) {
			if (!arg[5])  // No need to complain about subfolders as well.
				warn( "IMAP warning: ignoring INBOX in %s\n", ctx->prefix );
			goto skip;
		}
	}
	if (argl >= 5 && !memcmp( arg + argl - 5, ".lock", 5 )) /* workaround broken servers */
		goto skip;
	if (map_name( arg, (char **)&narg, offsetof(string_list_t, string), ctx->delimiter, "/") < 0) {
		warn( "IMAP warning: ignoring mailbox %s (reserved character '/' in name)\n", arg );
		goto skip;
	}
	narg->next = ctx->boxes;
	ctx->boxes = narg;
  skip:
	free_list( list );
	return LIST_OK;
}

static int
prepare_name( char **buf, const imap_store_t *ctx, const char *prefix, const char *name )
{
	int pl = strlen( prefix );

	switch (map_name( name, buf, pl, "/", ctx->delimiter )) {
	case -1:
		error( "IMAP error: mailbox name %s contains server's hierarchy delimiter\n", name );
		return -1;
	case -2:
		error( "IMAP error: server's hierarchy delimiter not known\n" );
		return -1;
	default:
		memcpy( *buf, prefix, pl );
		return 0;
	}
}

static int
prepare_box( char **buf, const imap_store_t *ctx )
{
	const char *name = ctx->name;

	return prepare_name( buf, ctx,
	    (starts_with( name, -1, "INBOX", 5 ) && (!name[5] || name[5] == '/')) ? "" : ctx->prefix, name );
}

static int
prepare_trash( char **buf, const imap_store_t *ctx )
{
	return prepare_name( buf, ctx, ctx->prefix, ctx->gen.conf->trash );
}

typedef struct {
	imap_cmd_t gen;
	imap_cmd_t *orig_cmd;
} imap_cmd_trycreate_t;

static void imap_open_store_greeted( imap_store_t * );
static void get_cmd_result_p2( imap_store_t *, imap_cmd_t *, int );

static void
imap_socket_read( void *aux )
{
	imap_store_t *ctx = (imap_store_t *)aux;
	imap_cmd_t *cmdp, **pcmdp;
	char *cmd, *arg, *arg1, *p;
	int resp, resp2, tag;
	conn_iovec_t iov[2];

	for (;;) {
		if (ctx->parse_list_sts.level) {
			resp = parse_list_continue( ctx, 0 );
		  listret:
			if (resp == LIST_PARTIAL)
				return;
			if (resp == LIST_BAD)
				break;
			continue;
		}
		if (!(cmd = socket_read_line( &ctx->conn )))
			return;
		if (cmd == (void *)~0) {
			if (!ctx->expectEOF)
				error( "IMAP error: unexpected EOF from %s\n", ctx->conn.name );
			/* A clean shutdown sequence ends with bad_callback as well (see imap_cleanup()). */
			break;
		}
		if (DFlags & DEBUG_NET) {
			printf( "%s%s\n", ctx->label, cmd );
			fflush( stdout );
		}

		arg = next_arg( &cmd );
		if (!arg) {
			error( "IMAP error: empty response\n" );
			break;
		}
		if (*arg == '*') {
			arg = next_arg( &cmd );
			if (!arg) {
				error( "IMAP error: malformed untagged response\n" );
				break;
			}

			if (ctx->greeting == GreetingPending && !strcmp( "PREAUTH", arg )) {
				parse_response_code( ctx, 0, cmd );
				ctx->greeting = GreetingPreauth;
			  dogreet:
				imap_ref( ctx );
				imap_open_store_greeted( ctx );
				if (imap_deref( ctx ))
					return;
			} else if (!strcmp( "OK", arg )) {
				parse_response_code( ctx, 0, cmd );
				if (ctx->greeting == GreetingPending) {
					ctx->greeting = GreetingOk;
					goto dogreet;
				}
			} else if (!strcmp( "BYE", arg )) {
				if (!ctx->expectBYE) {
					ctx->greeting = GreetingBad;
					error( "IMAP error: unexpected BYE response: %s\n", cmd );
					/* We just wait for the server to close the connection now. */
					ctx->expectEOF = 1;
				} else {
					/* We still need to wait for the LOGOUT's tagged OK. */
				}
			} else if (ctx->greeting == GreetingPending) {
				error( "IMAP error: bogus greeting response %s\n", arg );
				break;
			} else if (!strcmp( "NO", arg )) {
				warn( "Warning from IMAP server: %s\n", cmd );
			} else if (!strcmp( "BAD", arg )) {
				error( "Error from IMAP server: %s\n", cmd );
			} else if (!strcmp( "CAPABILITY", arg )) {
				parse_capability( ctx, cmd );
			} else if (!strcmp( "LIST", arg )) {
				resp = parse_list( ctx, cmd, parse_list_rsp );
				goto listret;
			} else if (!strcmp( "NAMESPACE", arg )) {
				resp = parse_list( ctx, cmd, parse_namespace_rsp );
				goto listret;
			} else if ((arg1 = next_arg( &cmd ))) {
				if (!strcmp( "EXISTS", arg1 ))
					ctx->total_msgs = atoi( arg );
				else if (!strcmp( "RECENT", arg1 ))
					ctx->recent_msgs = atoi( arg );
				else if(!strcmp ( "FETCH", arg1 )) {
					resp = parse_list( ctx, cmd, parse_fetch_rsp );
					goto listret;
				}
			} else {
				error( "IMAP error: unrecognized untagged response '%s'\n", arg );
				break; /* this may mean anything, so prefer not to spam the log */
			}
			continue;
		} else if (!ctx->in_progress) {
			error( "IMAP error: unexpected reply: %s %s\n", arg, cmd ? cmd : "" );
			break; /* this may mean anything, so prefer not to spam the log */
		} else if (*arg == '+') {
			socket_expect_read( &ctx->conn, 0 );
			/* There can be any number of commands in flight, but only the last
			 * one can require a continuation, as it enforces a round-trip. */
			cmdp = (imap_cmd_t *)((char *)ctx->in_progress_append -
			                      offsetof(imap_cmd_t, next));
			if (cmdp->param.data) {
				if (cmdp->param.to_trash)
					ctx->trashnc = TrashKnown; /* Can't get NO [TRYCREATE] any more. */
				if (DFlags & DEBUG_NET_ALL) {
					printf( "%s>>>>>>>>>\n", ctx->label );
					fwrite( cmdp->param.data, cmdp->param.data_len, 1, stdout );
					printf( "%s>>>>>>>>>\n", ctx->label );
					fflush( stdout );
				}
				iov[0].buf = cmdp->param.data;
				iov[0].len = cmdp->param.data_len;
				iov[0].takeOwn = GiveOwn;
				cmdp->param.data = 0;
				ctx->buffer_mem -= cmdp->param.data_len;
				iov[1].buf = "\r\n";
				iov[1].len = 2;
				iov[1].takeOwn = KeepOwn;
				socket_write( &ctx->conn, iov, 2 );
			} else if (cmdp->param.cont) {
				if (cmdp->param.cont( ctx, cmdp, cmd ))
					return;
			} else {
				error( "IMAP error: unexpected command continuation request\n" );
				break;
			}
			socket_expect_read( &ctx->conn, 1 );
		} else {
			tag = atoi( arg );
			for (pcmdp = &ctx->in_progress; (cmdp = *pcmdp); pcmdp = &cmdp->next)
				if (cmdp->tag == tag)
					goto gottag;
			error( "IMAP error: unexpected tag %s\n", arg );
			break;
		  gottag:
			if (!(*pcmdp = cmdp->next))
				ctx->in_progress_append = pcmdp;
			if (!--ctx->num_in_progress)
				socket_expect_read( &ctx->conn, 0 );
			arg = next_arg( &cmd );
			if (!arg) {
				error( "IMAP error: malformed tagged response\n" );
				break;
			}
			if (!strcmp( "OK", arg )) {
				if (cmdp->param.to_trash)
					ctx->trashnc = TrashKnown; /* Can't get NO [TRYCREATE] any more. */
				resp = RESP_OK;
			} else {
				if (!strcmp( "NO", arg )) {
					if (cmdp->param.create && cmd && starts_with( cmd, -1, "[TRYCREATE]", 11 )) { /* APPEND or UID COPY */
						imap_cmd_trycreate_t *cmd2 =
							(imap_cmd_trycreate_t *)new_imap_cmd( sizeof(*cmd2) );
						cmd2->orig_cmd = cmdp;
						cmd2->gen.param.high_prio = 1;
						p = strchr( cmdp->cmd, '"' );
						imap_exec( ctx, &cmd2->gen, get_cmd_result_p2,
						           "CREATE %.*s", imap_strchr( p + 1, '"' ) - p + 1, p );
						continue;
					}
					resp = RESP_NO;
					if (cmdp->param.failok)
						goto doresp;
				} else /*if (!strcmp( "BAD", arg ))*/
					resp = RESP_CANCEL;
				error( "IMAP command '%s' returned an error: %s %s\n",
				       starts_with( cmdp->cmd, -1, "LOGIN", 5 ) ?
				           "LOGIN <user> <pass>" :
				           starts_with( cmdp->cmd, -1, "AUTHENTICATE PLAIN", 18 ) ?
				               "AUTHENTICATE PLAIN <authdata>" :
				                cmdp->cmd,
				       arg, cmd ? cmd : "" );
			}
		  doresp:
			if ((resp2 = parse_response_code( ctx, cmdp, cmd )) > resp)
				resp = resp2;
			imap_ref( ctx );
			if (resp == RESP_CANCEL)
				imap_invoke_bad_callback( ctx );
			done_imap_cmd( ctx, cmdp, resp );
			if (imap_deref( ctx ))
				return;
			if (ctx->canceling && !ctx->in_progress) {
				ctx->canceling = 0;
				ctx->callbacks.imap_cancel( ctx->callback_aux );
				return;
			}
		}
		flush_imap_cmds( ctx );
	}
	imap_invoke_bad_callback( ctx );
}

static void
get_cmd_result_p2( imap_store_t *ctx, imap_cmd_t *cmd, int response )
{
	imap_cmd_trycreate_t *cmdp = (imap_cmd_trycreate_t *)cmd;
	imap_cmd_t *ocmd = cmdp->orig_cmd;

	if (response != RESP_OK) {
		done_imap_cmd( ctx, ocmd, response );
	} else {
		ctx->uidnext = 1;
		if (ocmd->param.to_trash)
			ctx->trashnc = TrashKnown;
		ocmd->param.create = 0;
		ocmd->param.high_prio = 1;
		submit_imap_cmd( ctx, ocmd );
	}
}

/******************* imap_cancel_store *******************/


static void
imap_cleanup_store( imap_store_t *ctx )
{
	free_generic_messages( ctx->msgs );
	free_string_list( ctx->boxes );
}

static void
imap_cancel_store( store_t *gctx )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

#ifdef HAVE_LIBSASL
	sasl_dispose( &ctx->sasl );
#endif
	socket_close( &ctx->conn );
	cancel_sent_imap_cmds( ctx );
	cancel_pending_imap_cmds( ctx );
	free_list( ctx->ns_personal );
	free_list( ctx->ns_other );
	free_list( ctx->ns_shared );
	free_string_list( ctx->auth_mechs );
	imap_cleanup_store( ctx );
	imap_deref( ctx );
}

static int
imap_deref( imap_store_t *ctx )
{
	if (!--ctx->ref_count) {
		free( ctx );
		return -1;
	}
	return 0;
}

static void
imap_set_bad_callback( store_t *gctx, void (*cb)( void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	ctx->bad_callback = cb;
	ctx->bad_callback_aux = aux;
}

static void
imap_invoke_bad_callback( imap_store_t *ctx )
{
	ctx->bad_callback( ctx->bad_callback_aux );
}

/******************* imap_free_store *******************/

static store_t *unowned;

static void
imap_cancel_unowned( void *gctx )
{
	store_t *store, **storep;

	for (storep = &unowned; (store = *storep); storep = &store->next)
		if (store == gctx) {
			*storep = store->next;
			break;
		}
	imap_cancel_store( gctx );
}

static void
imap_free_store( store_t *gctx )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	free_generic_messages( ctx->msgs );
	ctx->msgs = 0;
	imap_set_bad_callback( gctx, imap_cancel_unowned, gctx );
	gctx->next = unowned;
	unowned = gctx;
}

/******************* imap_cleanup *******************/

static void imap_cleanup_p2( imap_store_t *, imap_cmd_t *, int );

static void
imap_cleanup( void )
{
	store_t *ctx, *nctx;

	for (ctx = unowned; ctx; ctx = nctx) {
		nctx = ctx->next;
		imap_set_bad_callback( ctx, (void (*)(void *))imap_cancel_store, ctx );
		if (((imap_store_t *)ctx)->state != SST_BAD) {
			((imap_store_t *)ctx)->expectBYE = 1;
			imap_exec( (imap_store_t *)ctx, 0, imap_cleanup_p2, "LOGOUT" );
		} else {
			imap_cancel_store( ctx );
		}
	}
}

static void
imap_cleanup_p2( imap_store_t *ctx,
                 imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO)
		imap_cancel_store( &ctx->gen );
	else if (response == RESP_OK)
		ctx->expectEOF = 1;
}

/******************* imap_open_store *******************/

static void imap_open_store_connected( int, void * );
#ifdef HAVE_LIBSSL
static void imap_open_store_tlsstarted1( int, void * );
#endif
static void imap_open_store_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_open_store_authenticate( imap_store_t * );
#ifdef HAVE_LIBSSL
static void imap_open_store_authenticate_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_open_store_tlsstarted2( int, void * );
static void imap_open_store_authenticate_p3( imap_store_t *, imap_cmd_t *, int );
#endif
static void imap_open_store_authenticate2( imap_store_t * );
static void imap_open_store_authenticate2_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_open_store_compress( imap_store_t * );
#ifdef HAVE_LIBZ
static void imap_open_store_compress_p2( imap_store_t *, imap_cmd_t *, int );
#endif
static void imap_open_store_namespace( imap_store_t * );
static void imap_open_store_namespace_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_open_store_namespace2( imap_store_t * );
static void imap_open_store_finalize( imap_store_t * );
#ifdef HAVE_LIBSSL
static void imap_open_store_ssl_bail( imap_store_t * );
#endif
static void imap_open_store_bail( imap_store_t *, int );

static store_t *
imap_alloc_store( store_conf_t *conf, const char *label )
{
	imap_store_conf_t *cfg = (imap_store_conf_t *)conf;
	imap_server_conf_t *srvc = cfg->server;
	imap_store_t *ctx;
	store_t **ctxp;

	/* First try to recycle a whole store. */
	for (ctxp = &unowned; (ctx = (imap_store_t *)*ctxp); ctxp = &ctx->gen.next)
		if (ctx->state == SST_GOOD && ctx->gen.conf == conf) {
			*ctxp = ctx->gen.next;
			ctx->label = label;
			return &ctx->gen;
		}

	/* Then try to recycle a server connection. */
	for (ctxp = &unowned; (ctx = (imap_store_t *)*ctxp); ctxp = &ctx->gen.next)
		if (ctx->state != SST_BAD && ((imap_store_conf_t *)ctx->gen.conf)->server == srvc) {
			*ctxp = ctx->gen.next;
			imap_cleanup_store( ctx );
			/* One could ping the server here, but given that the idle timeout
			 * is at least 30 minutes, this sounds pretty pointless. */
			ctx->state = SST_HALF;
			goto gotsrv;
		}

	/* Finally, schedule opening a new server connection. */
	ctx = nfcalloc( sizeof(*ctx) );
	socket_init( &ctx->conn, &srvc->sconf,
	             (void (*)( void * ))imap_invoke_bad_callback,
	             imap_socket_read, (void (*)(void *))flush_imap_cmds, ctx );
	ctx->in_progress_append = &ctx->in_progress;
	ctx->pending_append = &ctx->pending;

  gotsrv:
	ctx->gen.driver = &imap_driver;
	ctx->gen.conf = conf;
	ctx->label = label;
	ctx->ref_count = 1;
	return &ctx->gen;
}

static void
imap_connect_store( store_t *gctx,
                    void (*cb)( int sts, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	if (ctx->state == SST_GOOD) {
		cb( DRV_OK, aux );
	} else {
		ctx->callbacks.imap_open = cb;
		ctx->callback_aux = aux;
		if (ctx->state == SST_HALF)
			imap_open_store_namespace( ctx );
		else
			socket_connect( &ctx->conn, imap_open_store_connected );
	}
}

static void
imap_open_store_connected( int ok, void *aux )
{
	imap_store_t *ctx = (imap_store_t *)aux;
#ifdef HAVE_LIBSSL
	imap_store_conf_t *cfg = (imap_store_conf_t *)ctx->gen.conf;
	imap_server_conf_t *srvc = cfg->server;
#endif

	if (!ok)
		imap_open_store_bail( ctx, FAIL_WAIT );
#ifdef HAVE_LIBSSL
	else if (srvc->ssl_type == SSL_IMAPS)
		socket_start_tls( &ctx->conn, imap_open_store_tlsstarted1 );
#endif
	else
		socket_expect_read( &ctx->conn, 1 );
}

#ifdef HAVE_LIBSSL
static void
imap_open_store_tlsstarted1( int ok, void *aux )
{
	imap_store_t *ctx = (imap_store_t *)aux;

	if (!ok)
		imap_open_store_ssl_bail( ctx );
	else
		socket_expect_read( &ctx->conn, 1 );
}
#endif

static void
imap_open_store_greeted( imap_store_t *ctx )
{
	socket_expect_read( &ctx->conn, 0 );
	if (!ctx->caps)
		imap_exec( ctx, 0, imap_open_store_p2, "CAPABILITY" );
	else
		imap_open_store_authenticate( ctx );
}

static void
imap_open_store_p2( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO)
		imap_open_store_bail( ctx, FAIL_FINAL );
	else if (response == RESP_OK)
		imap_open_store_authenticate( ctx );
}

static void
imap_open_store_authenticate( imap_store_t *ctx )
{
#ifdef HAVE_LIBSSL
	imap_store_conf_t *cfg = (imap_store_conf_t *)ctx->gen.conf;
	imap_server_conf_t *srvc = cfg->server;
#endif

	if (ctx->greeting != GreetingPreauth) {
#ifdef HAVE_LIBSSL
		if (srvc->ssl_type == SSL_STARTTLS) {
			if (CAP(STARTTLS)) {
				imap_exec( ctx, 0, imap_open_store_authenticate_p2, "STARTTLS" );
				return;
			} else {
				error( "IMAP error: SSL support not available\n" );
				imap_open_store_bail( ctx, FAIL_FINAL );
				return;
			}
		}
#endif
		imap_open_store_authenticate2( ctx );
	} else {
#ifdef HAVE_LIBSSL
		if (srvc->ssl_type == SSL_STARTTLS) {
			error( "IMAP error: SSL support not available\n" );
			imap_open_store_bail( ctx, FAIL_FINAL );
			return;
		}
#endif
		imap_open_store_compress( ctx );
	}
}

#ifdef HAVE_LIBSSL
static void
imap_open_store_authenticate_p2( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO)
		imap_open_store_bail( ctx, FAIL_FINAL );
	else if (response == RESP_OK)
		socket_start_tls( &ctx->conn, imap_open_store_tlsstarted2 );
}

static void
imap_open_store_tlsstarted2( int ok, void *aux )
{
	imap_store_t *ctx = (imap_store_t *)aux;

	if (!ok)
		imap_open_store_ssl_bail( ctx );
	else
		imap_exec( ctx, 0, imap_open_store_authenticate_p3, "CAPABILITY" );
}

static void
imap_open_store_authenticate_p3( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO)
		imap_open_store_bail( ctx, FAIL_FINAL );
	else if (response == RESP_OK)
		imap_open_store_authenticate2( ctx );
}
#endif

static const char *
ensure_user( imap_server_conf_t *srvc )
{
	if (!srvc->user) {
		error( "Skipping account %s, no user\n", srvc->name );
		return 0;
	}
	return srvc->user;
}

static const char *
ensure_password( imap_server_conf_t *srvc )
{
	char *cmd = srvc->pass_cmd;

	if (cmd) {
		FILE *fp;
		int ret;
		char buffer[2048];  // Hopefully more than enough room for XOAUTH2, etc. tokens

		if (*cmd == '+') {
			flushn();
			cmd++;
		}
		if (!(fp = popen( cmd, "r" ))) {
		  pipeerr:
			sys_error( "Skipping account %s, password command failed", srvc->name );
			return 0;
		}
		if (!fgets( buffer, sizeof(buffer), fp ))
			buffer[0] = 0;
		if ((ret = pclose( fp )) < 0)
			goto pipeerr;
		if (ret) {
			if (WIFSIGNALED( ret ))
				error( "Skipping account %s, password command crashed\n", srvc->name );
			else
				error( "Skipping account %s, password command exited with status %d\n", srvc->name, WEXITSTATUS( ret ) );
			return 0;
		}
		if (!buffer[0]) {
			error( "Skipping account %s, password command produced no output\n", srvc->name );
			return 0;
		}
		buffer[strcspn( buffer, "\n" )] = 0; /* Strip trailing newline */
		free( srvc->pass ); /* From previous runs */
		srvc->pass = nfstrdup( buffer );
	} else if (!srvc->pass) {
		char *pass, prompt[80];

		flushn();
		sprintf( prompt, "Password (%s): ", srvc->name );
		pass = getpass( prompt );
		if (!pass) {
			perror( "getpass" );
			exit( 1 );
		}
		if (!*pass) {
			error( "Skipping account %s, no password\n", srvc->name );
			return 0;
		}
		/* getpass() returns a pointer to a static buffer. Make a copy for long term storage. */
		srvc->pass = nfstrdup( pass );
	}
	return srvc->pass;
}

#ifdef HAVE_LIBSASL

static sasl_callback_t sasl_callbacks[] = {
	{ SASL_CB_USER,     NULL, NULL },
	{ SASL_CB_AUTHNAME, NULL, NULL },
	{ SASL_CB_PASS,     NULL, NULL },
	{ SASL_CB_LIST_END, NULL, NULL }
};

static int
process_sasl_interact( sasl_interact_t *interact, imap_server_conf_t *srvc )
{
	const char *val;

	for (;; ++interact) {
		switch (interact->id) {
		case SASL_CB_LIST_END:
			return 0;
		case SASL_CB_USER:
		case SASL_CB_AUTHNAME:
			val = ensure_user( srvc );
			break;
		case SASL_CB_PASS:
			val = ensure_password( srvc );
			break;
		default:
			error( "Error: Unknown SASL interaction ID\n" );
			return -1;
		}
		if (!val)
			return -1;
		interact->result = val;
		interact->len = strlen( val );
	}
}

static int
process_sasl_step( imap_store_t *ctx, int rc, const char *in, uint in_len,
                   sasl_interact_t *interact, const char **out, uint *out_len )
{
	imap_server_conf_t *srvc = ((imap_store_conf_t *)ctx->gen.conf)->server;

	while (rc == SASL_INTERACT) {
		if (process_sasl_interact( interact, srvc ) < 0)
			return -1;
		rc = sasl_client_step( ctx->sasl, in, in_len, &interact, out, out_len );
	}
	if (rc == SASL_CONTINUE) {
		ctx->sasl_cont = 1;
	} else if (rc == SASL_OK) {
		ctx->sasl_cont = 0;
	} else {
		error( "Error: %s\n", sasl_errdetail( ctx->sasl ) );
		return -1;
	}
	return 0;
}

static int
decode_sasl_data( const char *prompt, char **in, uint *in_len )
{
	if (prompt) {
		int rc;
		uint prompt_len = strlen( prompt );
		/* We're decoding, the output will be shorter than prompt_len. */
		*in = nfmalloc( prompt_len );
		rc = sasl_decode64( prompt, prompt_len, *in, prompt_len, in_len );
		if (rc != SASL_OK) {
			free( *in );
			error( "Error: SASL(%d): %s\n", rc, sasl_errstring( rc, NULL, NULL ) );
			return -1;
		}
	} else {
		*in = NULL;
		*in_len = 0;
	}
	return 0;
}

static int
encode_sasl_data( const char *out, uint out_len, char **enc, uint *enc_len )
{
	int rc;
	uint enc_len_max = ((out_len + 2) / 3) * 4 + 1;
	*enc = nfmalloc( enc_len_max );
	rc = sasl_encode64( out, out_len, *enc, enc_len_max, enc_len );
	if (rc != SASL_OK) {
		free( *enc );
		error( "Error: SASL(%d): %s\n", rc, sasl_errstring( rc, NULL, NULL ) );
		return -1;
	}
	return 0;
}

static int
do_sasl_auth( imap_store_t *ctx, imap_cmd_t *cmdp ATTR_UNUSED, const char *prompt )
{
	int rc, ret, iovcnt = 0;
	uint in_len, out_len, enc_len;
	const char *out;
	char *in, *enc;
	sasl_interact_t *interact = NULL;
	conn_iovec_t iov[2];

	if (!ctx->sasl_cont) {
		error( "Error: IMAP wants more steps despite successful SASL authentication.\n" );
		goto bail;
	}
	if (decode_sasl_data( prompt, &in, &in_len ) < 0)
		goto bail;
	rc = sasl_client_step( ctx->sasl, in, in_len, &interact, &out, &out_len );
	ret = process_sasl_step( ctx, rc, in, in_len, interact, &out, &out_len );
	free( in );
	if (ret < 0)
		goto bail;

	if (out) {
		if (encode_sasl_data( out, out_len, &enc, &enc_len ) < 0)
			goto bail;

		iov[0].buf = enc;
		iov[0].len = enc_len;
		iov[0].takeOwn = GiveOwn;
		iovcnt = 1;

		if (DFlags & DEBUG_NET) {
			printf( "%s>+> %s\n", ctx->label, enc );
			fflush( stdout );
		}
	} else {
		if (DFlags & DEBUG_NET) {
			printf( "%s>+>\n", ctx->label );
			fflush( stdout );
		}
	}
	iov[iovcnt].buf = "\r\n";
	iov[iovcnt].len = 2;
	iov[iovcnt].takeOwn = KeepOwn;
	iovcnt++;
	socket_write( &ctx->conn, iov, iovcnt );
	return 0;

  bail:
	imap_open_store_bail( ctx, FAIL_FINAL );
	return -1;
}

static void
done_sasl_auth( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_OK && ctx->sasl_cont) {
		sasl_interact_t *interact = NULL;
		const char *out;
		uint out_len;
		int rc = sasl_client_step( ctx->sasl, NULL, 0, &interact, &out, &out_len );
		if (process_sasl_step( ctx, rc, NULL, 0, interact, &out, &out_len ) < 0)
			warn( "Warning: SASL reported failure despite successful IMAP authentication. Ignoring...\n" );
		else if (out_len > 0)
			warn( "Warning: SASL wants more steps despite successful IMAP authentication. Ignoring...\n" );
	}

	imap_open_store_authenticate2_p2( ctx, NULL, response );
}

#endif

static size_t
encoded_length( size_t in )
{
	in += 2;
	in /= 3;
	in <<= 2;
	in++;

	return in;
}

static int
auth_builtin_oauthbearer( imap_store_t *ctx )
{
	imap_store_conf_t *cfg = (imap_store_conf_t *)ctx->gen.conf;
	imap_server_conf_t *srvc = cfg->server;
	size_t buf_size = 38; /* format string, port, NULL */
	size_t input_size;
	char *buf, *buf_encoded;

#ifdef HAVE_LIBSSL
	if (!ctx->conn.ssl) {
#endif
		error( "Note: not using OAUTHBEARER because connection is not encrypted;\n" );
		return -1;
#ifdef HAVE_LIBSSL
	}
#endif

	if (!ensure_user( srvc ) || !ensure_password( srvc ))
		return -1;

	buf_size += strlen( srvc->user );
	buf_size += strlen( srvc->sconf.host );
	buf_size += strlen( srvc->pass );

	buf = nfmalloc( buf_size );
	input_size = nfsnprintf( buf, buf_size, "n,a=%s,\001host=%s\001port=%d\001auth=Bearer %s\001\001",
	            srvc->user, srvc->sconf.host, srvc->sconf.port, srvc->pass );

	buf_encoded = nfmalloc ( encoded_length( input_size ) );

#ifdef HAVE_LIBSSL
	EVP_EncodeBlock( (unsigned char *)buf_encoded, (unsigned char *)buf, input_size );
#endif

	imap_exec( ctx, 0, imap_open_store_authenticate2_p2,
	           "AUTHENTICATE OAUTHBEARER %s", buf_encoded );

	free ( buf_encoded );
	free ( buf );

	return 0;
}

static int
auth_builtin_login( imap_store_t *ctx )
{
	imap_store_conf_t *cfg = (imap_store_conf_t *)ctx->gen.conf;
	imap_server_conf_t *srvc = cfg->server;

	if (!ensure_user( srvc ) || !ensure_password( srvc ))
		return -1;

#ifdef HAVE_LIBSSL
	if (!ctx->conn.ssl)
#endif
		warn( "*** IMAP Warning *** Password is being sent in the clear\n" );
	imap_exec( ctx, 0, imap_open_store_authenticate2_p2,
		   "LOGIN \"%\\s\" \"%\\s\"", srvc->user, srvc->pass );

	return 0;
}

#define AUTH_BUILTIN_LOGIN		0x1
#define AUTH_BUILTIN_OAUTHBEARER	0x2

static void
imap_open_store_authenticate2( imap_store_t *ctx )
{
	imap_store_conf_t *cfg = (imap_store_conf_t *)ctx->gen.conf;
	imap_server_conf_t *srvc = cfg->server;
	string_list_t *mech, *cmech;
	int auth_builtin = 0;
	int skipped_builtin = 0;
#ifdef HAVE_LIBSASL
	const char *saslavail;
	char saslmechs[1024], *saslend = saslmechs;
#endif

	info( "Logging in...\n" );
	for (mech = srvc->auth_mechs; mech; mech = mech->next) {
		int any = !strcmp( mech->string, "*" );
		for (cmech = ctx->auth_mechs; cmech; cmech = cmech->next) {
			if (any || !strcasecmp( mech->string, cmech->string )) {
				if (!strcasecmp( cmech->string, "LOGIN" ))
					auth_builtin |= AUTH_BUILTIN_LOGIN;
				if (!strcasecmp( cmech->string, "OAUTHBEARER" ))
					auth_builtin |= AUTH_BUILTIN_OAUTHBEARER;
				if (auth_builtin & (AUTH_BUILTIN_LOGIN | AUTH_BUILTIN_OAUTHBEARER)) {
#ifdef HAVE_LIBSSL
					if (!ctx->conn.ssl && any) {
#else
					if (any) {
#endif
						auth_builtin = 0;
						skipped_builtin = 1;
					}
#ifdef HAVE_LIBSASL
				} else {
					int len = strlen( cmech->string );
					if (saslend + len + 2 > saslmechs + sizeof(saslmechs))
						oob();
					*saslend++ = ' ';
					memcpy( saslend, cmech->string, len + 1 );
					saslend += len;
#endif
				}
			}
		}
	}
#ifdef HAVE_LIBSASL
	if (saslend != saslmechs) {
		int rc;
		uint out_len = 0;
		char *enc = NULL;
		const char *gotmech = NULL, *out = NULL;
		sasl_interact_t *interact = NULL;
		imap_cmd_t *cmd;
		static int sasl_inited;

		if (!sasl_inited) {
			rc = sasl_client_init( sasl_callbacks );
			if (rc != SASL_OK) {
			  saslbail:
				error( "Error: SASL(%d): %s\n", rc, sasl_errstring( rc, NULL, NULL ) );
				goto bail;
			}
			sasl_inited = 1;
		}

		rc = sasl_client_new( "imap", srvc->sconf.host, NULL, NULL, NULL, 0, &ctx->sasl );
		if (rc != SASL_OK) {
			if (rc == SASL_NOMECH)
				goto notsasl;
			if (!ctx->sasl)
				goto saslbail;
			error( "Error: %s\n", sasl_errdetail( ctx->sasl ) );
			goto bail;
		}

		rc = sasl_client_start( ctx->sasl, saslmechs + 1, &interact, CAP(SASLIR) ? &out : NULL, &out_len, &gotmech );
		if (rc == SASL_NOMECH)
			goto notsasl;
		if (gotmech)
			info( "Authenticating with SASL mechanism %s...\n", gotmech );
		/* Technically, we are supposed to loop over sasl_client_start(),
		 * but it just calls sasl_client_step() anyway. */
		if (process_sasl_step( ctx, rc, NULL, 0, interact, CAP(SASLIR) ? &out : NULL, &out_len ) < 0)
			goto bail;
		if (out) {
			if (!out_len)
				enc = nfstrdup( "=" ); /* A zero-length initial response is encoded as padding. */
			else if (encode_sasl_data( out, out_len, &enc, NULL ) < 0)
				goto bail;
		}

		cmd = new_imap_cmd( sizeof(*cmd) );
		cmd->param.cont = do_sasl_auth;
		imap_exec( ctx, cmd, done_sasl_auth, enc ? "AUTHENTICATE %s %s" : "AUTHENTICATE %s", gotmech, enc );
		free( enc );
		return;
	  notsasl:
		if (!ctx->sasl || sasl_listmech( ctx->sasl, NULL, "", " ", "", &saslavail, NULL, NULL ) != SASL_OK)
			saslavail = "(none)";  /* EXTERNAL is always there anyway. */
		if (!auth_builtin) {
			error( "IMAP error: selected SASL mechanism(s) not available;\n"
			       "   selected:%s\n   available: %s\n", saslmechs, saslavail );
			goto skipnote;
		}
		info( "NOT using available SASL mechanism(s): %s\n", saslavail );
		sasl_dispose( &ctx->sasl );
	}
#endif
	if (auth_builtin & AUTH_BUILTIN_LOGIN) {
		if ( !auth_builtin_login( ctx ))
			return;
	}
	if (auth_builtin & AUTH_BUILTIN_OAUTHBEARER) {
		if ( !auth_builtin_oauthbearer( ctx ))
			return;
	}
	error( "IMAP error: server supports no acceptable authentication mechanism\n" );
#ifdef HAVE_LIBSASL
  skipnote:
#endif
	if (skipped_builtin)
		error( "Note: not using builtin authentication because connection is not encrypted;\n"
		       "      use e.g. 'AuthMechs LOGIN' explicitly to force it.\n" );

  bail:
	imap_open_store_bail( ctx, FAIL_FINAL );
}

static void
imap_open_store_authenticate2_p2( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO)
		imap_open_store_bail( ctx, FAIL_FINAL );
	else if (response == RESP_OK)
		imap_open_store_compress( ctx );
}

static void
imap_open_store_compress( imap_store_t *ctx )
{
#ifdef HAVE_LIBZ
	if (CAP(COMPRESS_DEFLATE)) {
		imap_exec( ctx, 0, imap_open_store_compress_p2, "COMPRESS DEFLATE" );
		return;
	}
#endif
	imap_open_store_namespace( ctx );
}

#ifdef HAVE_LIBZ
static void
imap_open_store_compress_p2( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO) {
		/* We already reported an error, but it's not fatal to us. */
		imap_open_store_namespace( ctx );
	} else if (response == RESP_OK) {
		socket_start_deflate( &ctx->conn );
		imap_open_store_namespace( ctx );
	}
}
#endif

static void
imap_open_store_namespace( imap_store_t *ctx )
{
	imap_store_conf_t *cfg = (imap_store_conf_t *)ctx->gen.conf;

	ctx->state = SST_HALF;
	ctx->prefix = cfg->gen.path;
	ctx->delimiter[0] = cfg->delimiter;
	if (((!ctx->prefix && cfg->use_namespace) || !cfg->delimiter) && CAP(NAMESPACE)) {
		/* get NAMESPACE info */
		if (!ctx->got_namespace)
			imap_exec( ctx, 0, imap_open_store_namespace_p2, "NAMESPACE" );
		else
			imap_open_store_namespace2( ctx );
		return;
	}
	imap_open_store_finalize( ctx );
}

static void
imap_open_store_namespace_p2( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO) {
		imap_open_store_bail( ctx, FAIL_FINAL );
	} else if (response == RESP_OK) {
		ctx->got_namespace = 1;
		imap_open_store_namespace2( ctx );
	}
}

static void
imap_open_store_namespace2( imap_store_t *ctx )
{
	imap_store_conf_t *cfg = (imap_store_conf_t *)ctx->gen.conf;
	list_t *nsp, *nsp_1st;

	/* XXX for now assume 1st personal namespace */
	if (is_list( (nsp = ctx->ns_personal) ) &&
	    is_list( (nsp_1st = nsp->child) ))
	{
		list_t *nsp_1st_ns = nsp_1st->child;
		list_t *nsp_1st_dl = nsp_1st_ns->next;
		if (!ctx->prefix && cfg->use_namespace)
			ctx->prefix = nsp_1st_ns->val;
		if (!ctx->delimiter[0] && is_atom( nsp_1st_dl ))
			ctx->delimiter[0] = nsp_1st_dl->val[0];
	}
	imap_open_store_finalize( ctx );
}

static void
imap_open_store_finalize( imap_store_t *ctx )
{
	ctx->state = SST_GOOD;
	if (!ctx->prefix)
		ctx->prefix = "";
	ctx->trashnc = TrashUnknown;
	ctx->callbacks.imap_open( DRV_OK, ctx->callback_aux );
}

#ifdef HAVE_LIBSSL
static void
imap_open_store_ssl_bail( imap_store_t *ctx )
{
	/* This avoids that we try to send LOGOUT to an unusable socket. */
	socket_close( &ctx->conn );
	imap_open_store_bail( ctx, FAIL_FINAL );
}
#endif

static void
imap_open_store_bail( imap_store_t *ctx, int failed )
{
	((imap_store_conf_t *)ctx->gen.conf)->server->failed = failed;
	ctx->callbacks.imap_open( DRV_STORE_BAD, ctx->callback_aux );
}

/******************* imap_open_box *******************/

static int
imap_select_box( store_t *gctx, const char *name )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	free_generic_messages( ctx->msgs );
	ctx->msgs = 0;
	ctx->msgapp = &ctx->msgs;

	ctx->name = name;
	return DRV_OK;
}

static const char *
imap_get_box_path( store_t *gctx ATTR_UNUSED )
{
	return 0;
}

typedef struct {
	imap_cmd_t gen;
	void (*callback)( int sts, int uidvalidity, void *aux );
	void *callback_aux;
} imap_cmd_open_box_t;

static void imap_open_box_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_open_box_p3( imap_store_t *, imap_cmd_t *, int );
static void imap_open_box_p4( imap_store_t *, imap_cmd_open_box_t *, int );

static void
imap_open_box( store_t *gctx,
               void (*cb)( int sts, int uidvalidity, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;
	imap_cmd_open_box_t *cmd;
	char *buf;

	if (prepare_box( &buf, ctx ) < 0) {
		cb( DRV_BOX_BAD, UIDVAL_BAD, aux );
		return;
	}

	ctx->uidvalidity = UIDVAL_BAD;
	ctx->uidnext = 0;

	INIT_IMAP_CMD(imap_cmd_open_box_t, cmd, cb, aux)
	cmd->gen.param.failok = 1;
	imap_exec( ctx, &cmd->gen, imap_open_box_p2,
	           "SELECT \"%\\s\"", buf );
	free( buf );
}

static void
imap_open_box_p2( imap_store_t *ctx, imap_cmd_t *gcmd, int response )
{
	imap_cmd_open_box_t *cmdp = (imap_cmd_open_box_t *)gcmd;
	imap_cmd_open_box_t *cmd;

	if (response != RESP_OK || ctx->uidnext) {
		imap_open_box_p4( ctx, cmdp, response );
		return;
	}

	INIT_IMAP_CMD(imap_cmd_open_box_t, cmd, cmdp->callback, cmdp->callback_aux)
	cmd->gen.param.lastuid = 1;
	imap_exec( ctx, &cmd->gen, imap_open_box_p3,
	           "UID FETCH * (UID)" );
}

static void
imap_open_box_p3( imap_store_t *ctx, imap_cmd_t *gcmd, int response )
{
	imap_cmd_open_box_t *cmdp = (imap_cmd_open_box_t *)gcmd;

	if (!ctx->uidnext) {
		if (ctx->total_msgs) {
			error( "IMAP error: querying server for highest UID failed\n" );
			imap_open_box_p4( ctx, cmdp, RESP_NO );
			return;
		}
		// This is ok, the box is simply empty.
		ctx->uidnext = 1;
	}

	imap_open_box_p4( ctx, cmdp, response );
}

static void
imap_open_box_p4( imap_store_t *ctx, imap_cmd_open_box_t *cmdp, int response )
{
	transform_box_response( &response );
	cmdp->callback( response, ctx->uidvalidity, cmdp->callback_aux );
}

static int
imap_get_uidnext( store_t *gctx )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	return ctx->uidnext;
}

/******************* imap_create_box *******************/

static void
imap_create_box( store_t *gctx,
                 void (*cb)( int sts, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;
	imap_cmd_simple_t *cmd;
	char *buf;

	if (prepare_box( &buf, ctx ) < 0) {
		cb( DRV_BOX_BAD, aux );
		return;
	}

	INIT_IMAP_CMD(imap_cmd_simple_t, cmd, cb, aux)
	imap_exec( ctx, &cmd->gen, imap_done_simple_box,
	           "CREATE \"%\\s\"", buf );
	free( buf );
}

/******************* imap_delete_box *******************/

static int
imap_confirm_box_empty( store_t *gctx )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	return ctx->total_msgs ? DRV_BOX_BAD : DRV_OK;
}

static void imap_delete_box_p2( imap_store_t *, imap_cmd_t *, int );

static void
imap_delete_box( store_t *gctx,
                 void (*cb)( int sts, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;
	imap_cmd_simple_t *cmd;

	INIT_IMAP_CMD(imap_cmd_simple_t, cmd, cb, aux)
	imap_exec( ctx, &cmd->gen, imap_delete_box_p2, "CLOSE" );
}

static void
imap_delete_box_p2( imap_store_t *ctx, imap_cmd_t *gcmd, int response )
{
	imap_cmd_simple_t *cmdp = (imap_cmd_simple_t *)gcmd;
	imap_cmd_simple_t *cmd;
	char *buf;

	if (response != RESP_OK) {
		imap_done_simple_box( ctx, &cmdp->gen, response );
		return;
	}

	if (prepare_box( &buf, ctx ) < 0) {
		imap_done_simple_box( ctx, &cmdp->gen, RESP_NO );
		return;
	}
	INIT_IMAP_CMD(imap_cmd_simple_t, cmd, cmdp->callback, cmdp->callback_aux)
	imap_exec( ctx, &cmd->gen, imap_done_simple_box,
	           "DELETE \"%\\s\"", buf );
	free( buf );
}

static int
imap_finish_delete_box( store_t *gctx ATTR_UNUSED )
{
	return DRV_OK;
}

/******************* imap_load_box *******************/

static int
imap_prepare_load_box( store_t *gctx, int opts )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	ctx->opts = opts;
	return opts;
}

enum { WantSize = 1, WantTuids = 2, WantMsgids = 4 };
typedef struct {
	int first, last, flags;
} imap_range_t;

static void
imap_set_range( imap_range_t *ranges, int *nranges, int low_flags, int high_flags, int maxlow )
{
	if (low_flags != high_flags) {
		for (int r = 0; r < *nranges; r++) {
			if (ranges[r].first > maxlow)
				break; /* Range starts above split point; so do all subsequent ranges. */
			if (ranges[r].last < maxlow)
				continue; /* Range ends below split point; try next one. */
			if (ranges[r].last != maxlow) {
				/* Range does not end exactly at split point; need to split. */
				memmove( &ranges[r + 1], &ranges[r], ((*nranges)++ - r) * sizeof(*ranges) );
				ranges[r].last = maxlow;
				ranges[r + 1].first = maxlow + 1;
			}
			break;
		}
	}
	for (int r = 0; r < *nranges; r++)
		ranges[r].flags |= (ranges[r].last <= maxlow) ? low_flags : high_flags;
}

typedef struct {
	imap_cmd_refcounted_state_t gen;
	void (*callback)( int sts, message_t *msgs, int total_msgs, int recent_msgs, void *aux );
	void *callback_aux;
} imap_load_box_state_t;

static void imap_submit_load( imap_store_t *, const char *, int, imap_load_box_state_t * );
static void imap_submit_load_p3( imap_store_t *ctx, imap_load_box_state_t * );

static void
imap_load_box( store_t *gctx, uint minuid, uint maxuid, uint newuid, uint seenuid, uint_array_t excs,
               void (*cb)( int sts, message_t *msgs, int total_msgs, int recent_msgs, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;
	int i, j, bl;
	char buf[1000];

	if (!ctx->total_msgs) {
		free( excs.data );
		cb( DRV_OK, 0, 0, 0, aux );
	} else {
		INIT_REFCOUNTED_STATE(imap_load_box_state_t, sts, cb, aux)
		for (i = 0; i < excs.size; ) {
			for (bl = 0; i < excs.size && bl < 960; i++) {
				if (bl)
					buf[bl++] = ',';
				bl += sprintf( buf + bl, "%u", excs.data[i] );
				j = i;
				for (; i + 1 < excs.size && excs.data[i + 1] == excs.data[i] + 1; i++) {}
				if (i != j)
					bl += sprintf( buf + bl, ":%u", excs.data[i] );
			}
			imap_submit_load( ctx, buf, shifted_bit( ctx->opts, OPEN_OLD_IDS, WantMsgids ), sts );
		}
		if (maxuid == UINT_MAX)
			maxuid = ctx->uidnext - 1;
		if (maxuid >= minuid) {
			imap_range_t ranges[3];
			ranges[0].first = minuid;
			ranges[0].last = maxuid;
			ranges[0].flags = 0;
			int nranges = 1;
			if (ctx->opts & (OPEN_OLD_SIZE | OPEN_NEW_SIZE))
				imap_set_range( ranges, &nranges, shifted_bit( ctx->opts, OPEN_OLD_SIZE, WantSize),
				                                  shifted_bit( ctx->opts, OPEN_NEW_SIZE, WantSize), seenuid );
			if (ctx->opts & OPEN_FIND)
				imap_set_range( ranges, &nranges, 0, WantTuids, newuid - 1 );
			if (ctx->opts & OPEN_OLD_IDS)
				imap_set_range( ranges, &nranges, WantMsgids, 0, seenuid );
			for (int r = 0; r < nranges; r++) {
				sprintf( buf, "%u:%u", ranges[r].first, ranges[r].last );
				imap_submit_load( ctx, buf, ranges[r].flags, sts );
			}
		}
		free( excs.data );
		imap_submit_load_p3( ctx, sts );
	}
}

static int
imap_sort_msgs_comp( const void *a_, const void *b_ )
{
	const message_t *a = *(const message_t * const *)a_;
	const message_t *b = *(const message_t * const *)b_;

	if (a->uid < b->uid)
		return -1;
	if (a->uid > b->uid)
		return 1;
	return 0;
}

static void
imap_sort_msgs( imap_store_t *ctx )
{
	int count = count_generic_messages( ctx->msgs );
	if (count <= 1)
		return;

	message_t **t = nfmalloc( sizeof(*t) * count );

	message_t *m = ctx->msgs;
	for (int i = 0; i < count; i++) {
		t[i] = m;
		m = m->next;
	}

	qsort( t, count, sizeof(*t), imap_sort_msgs_comp );

	ctx->msgs = t[0];

	int j;
	for (j = 0; j < count - 1; j++)
		t[j]->next = t[j + 1];
	ctx->msgapp = &t[j]->next;
	*ctx->msgapp = NULL;

	free( t );
}

static void imap_submit_load_p2( imap_store_t *, imap_cmd_t *, int );

static void
imap_submit_load( imap_store_t *ctx, const char *buf, int flags, imap_load_box_state_t *sts )
{
	imap_exec( ctx, imap_refcounted_new_cmd( &sts->gen ), imap_submit_load_p2,
	           "UID FETCH %s (UID%s%s%s%s%s%s%s)", buf,
	           (ctx->opts & OPEN_FLAGS) ? " FLAGS" : "",
	           (flags & WantSize) ? " RFC822.SIZE" : "",
	           (flags & (WantTuids | WantMsgids)) ? " BODY.PEEK[HEADER.FIELDS (" : "",
	           (flags & WantTuids) ? "X-TUID" : "",
	           !(~flags & (WantTuids | WantMsgids)) ? " " : "",
	           (flags & WantMsgids) ? "MESSAGE-ID" : "",
	           (flags & (WantTuids | WantMsgids)) ? ")]" : "");
}

static void
imap_submit_load_p2( imap_store_t *ctx, imap_cmd_t *cmd, int response )
{
	imap_load_box_state_t *sts = (imap_load_box_state_t *)((imap_cmd_refcounted_t *)cmd)->state;

	transform_refcounted_box_response( &sts->gen, response );
	imap_submit_load_p3( ctx, sts );
}

static void
imap_submit_load_p3( imap_store_t *ctx, imap_load_box_state_t *sts )
{
	DONE_REFCOUNTED_STATE_ARGS(sts, {
		if (sts->gen.ret_val == DRV_OK)
			imap_sort_msgs( ctx );
	}, ctx->msgs, ctx->total_msgs, ctx->recent_msgs)
}

/******************* imap_fetch_msg *******************/

static void imap_fetch_msg_p2( imap_store_t *, imap_cmd_t *, int );

static void
imap_fetch_msg( store_t *ctx, message_t *msg, msg_data_t *data,
                void (*cb)( int sts, void *aux ), void *aux )
{
	imap_cmd_fetch_msg_t *cmd;

	INIT_IMAP_CMD_X(imap_cmd_fetch_msg_t, cmd, cb, aux)
	cmd->gen.gen.param.uid = msg->uid;
	cmd->msg_data = data;
	data->data = 0;
	imap_exec( (imap_store_t *)ctx, &cmd->gen.gen, imap_fetch_msg_p2,
	           "UID FETCH %u (%s%sBODY.PEEK[])", msg->uid,
	           !(msg->status & M_FLAGS) ? "FLAGS " : "",
	           (data->date== -1) ? "INTERNALDATE " : "" );
}

static void
imap_fetch_msg_p2( imap_store_t *ctx, imap_cmd_t *gcmd, int response )
{
	imap_cmd_fetch_msg_t *cmd = (imap_cmd_fetch_msg_t *)gcmd;

	if (response == RESP_OK && !cmd->msg_data->data) {
		/* The FETCH succeeded, but there is no message with this UID. */
		response = RESP_NO;
	}
	imap_done_simple_msg( ctx, gcmd, response );
}

/******************* imap_set_msg_flags *******************/

static int
imap_make_flags( int flags, char *buf )
{
	const char *s;
	uint i, d;

	for (i = d = 0; i < as(Flags); i++)
		if (flags & (1 << i)) {
			buf[d++] = ' ';
			buf[d++] = '\\';
			for (s = Flags[i]; *s; s++)
				buf[d++] = *s;
		}
	buf[0] = '(';
	buf[d++] = ')';
	return d;
}

typedef struct {
	imap_cmd_refcounted_state_t gen;
	void (*callback)( int sts, void *aux );
	void *callback_aux;
} imap_set_msg_flags_state_t;

static void imap_set_flags_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_set_flags_p3( imap_set_msg_flags_state_t * );

static void
imap_flags_helper( imap_store_t *ctx, uint uid, char what, int flags,
                   imap_set_msg_flags_state_t *sts )
{
	char buf[256];

	buf[imap_make_flags( flags, buf )] = 0;
	imap_exec( ctx, imap_refcounted_new_cmd( &sts->gen ), imap_set_flags_p2,
	           "UID STORE %u %cFLAGS.SILENT %s", uid, what, buf );
}

static void
imap_set_msg_flags( store_t *gctx, message_t *msg, uint uid, int add, int del,
                    void (*cb)( int sts, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	if (msg) {
		uid = msg->uid;
		add &= ~msg->flags;
		del &= msg->flags;
		msg->flags |= add;
		msg->flags &= ~del;
	}
	if (add || del) {
		INIT_REFCOUNTED_STATE(imap_set_msg_flags_state_t, sts, cb, aux)
		if (add)
			imap_flags_helper( ctx, uid, '+', add, sts );
		if (del)
			imap_flags_helper( ctx, uid, '-', del, sts );
		imap_set_flags_p3( sts );
	} else {
		cb( DRV_OK, aux );
	}
}

static void
imap_set_flags_p2( imap_store_t *ctx ATTR_UNUSED, imap_cmd_t *cmd, int response )
{
	imap_set_msg_flags_state_t *sts = (imap_set_msg_flags_state_t *)((imap_cmd_refcounted_t *)cmd)->state;

	transform_refcounted_msg_response( &sts->gen, response);
	imap_set_flags_p3( sts );
}

static void
imap_set_flags_p3( imap_set_msg_flags_state_t *sts )
{
	DONE_REFCOUNTED_STATE(sts)
}

/******************* imap_close_box *******************/

typedef struct {
	imap_cmd_refcounted_state_t gen;
	void (*callback)( int sts, void *aux );
	void *callback_aux;
} imap_expunge_state_t;

static void imap_close_box_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_close_box_p3( imap_expunge_state_t * );

static void
imap_close_box( store_t *gctx,
                void (*cb)( int sts, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	if (ctx->gen.conf->trash && CAP(UIDPLUS)) {
		INIT_REFCOUNTED_STATE(imap_expunge_state_t, sts, cb, aux)
		message_t *msg, *fmsg, *nmsg;
		int bl;
		char buf[1000];

		for (msg = ctx->msgs; ; ) {
			for (bl = 0; msg && bl < 960; msg = msg->next) {
				if (!(msg->flags & F_DELETED))
					continue;
				if (bl)
					buf[bl++] = ',';
				bl += sprintf( buf + bl, "%u", msg->uid );
				fmsg = msg;
				for (; (nmsg = msg->next) && (nmsg->flags & F_DELETED); msg = nmsg) {}
				if (msg != fmsg)
					bl += sprintf( buf + bl, ":%u", msg->uid );
			}
			if (!bl)
				break;
			imap_exec( ctx, imap_refcounted_new_cmd( &sts->gen ), imap_close_box_p2,
			           "UID EXPUNGE %s", buf );
		}
		imap_close_box_p3( sts );
	} else {
		/* This is inherently racy: it may cause messages which other clients
		 * marked as deleted to be expunged without being trashed. */
		imap_cmd_simple_t *cmd;
		INIT_IMAP_CMD(imap_cmd_simple_t, cmd, cb, aux)
		imap_exec( ctx, &cmd->gen, imap_done_simple_box, "CLOSE" );
	}
}

static void
imap_close_box_p2( imap_store_t *ctx ATTR_UNUSED, imap_cmd_t *cmd, int response )
{
	imap_expunge_state_t *sts = (imap_expunge_state_t *)((imap_cmd_refcounted_t *)cmd)->state;

	transform_refcounted_box_response( &sts->gen, response );
	imap_close_box_p3( sts );
}

static void
imap_close_box_p3( imap_expunge_state_t *sts )
{
	DONE_REFCOUNTED_STATE(sts)
}

/******************* imap_trash_msg *******************/

static void
imap_trash_msg( store_t *gctx, message_t *msg,
                void (*cb)( int sts, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;
	imap_cmd_simple_t *cmd;
	char *buf;

	INIT_IMAP_CMD(imap_cmd_simple_t, cmd, cb, aux)
	cmd->gen.param.create = 1;
	cmd->gen.param.to_trash = 1;
	if (prepare_trash( &buf, ctx ) < 0) {
		cb( DRV_BOX_BAD, aux );
		return;
	}
	imap_exec( ctx, &cmd->gen, imap_done_simple_msg,
	           CAP(MOVE) ? "UID MOVE %u \"%\\s\"" : "UID COPY %u \"%\\s\"", msg->uid, buf );
	free( buf );
}

/******************* imap_store_msg *******************/

static void imap_store_msg_p2( imap_store_t *, imap_cmd_t *, int );

static size_t
my_strftime( char *s, size_t max, const char *fmt, const struct tm *tm )
{
    return strftime( s, max, fmt, tm );
}

static void
imap_store_msg( store_t *gctx, msg_data_t *data, int to_trash,
                void (*cb)( int sts, uint uid, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;
	imap_cmd_out_uid_t *cmd;
	char *buf;
	int d;
	char flagstr[128], datestr[64];

	d = 0;
	if (data->flags) {
		d = imap_make_flags( data->flags, flagstr );
		flagstr[d++] = ' ';
	}
	flagstr[d] = 0;

	INIT_IMAP_CMD(imap_cmd_out_uid_t, cmd, cb, aux)
	ctx->buffer_mem += data->len;
	cmd->gen.param.data_len = data->len;
	cmd->gen.param.data = data->data;
	cmd->out_uid = 0;

	if (to_trash) {
		cmd->gen.param.create = 1;
		cmd->gen.param.to_trash = 1;
		if (prepare_trash( &buf, ctx ) < 0) {
			cb( DRV_BOX_BAD, -1, aux );
			return;
		}
	} else {
		if (prepare_box( &buf, ctx ) < 0) {
			cb( DRV_BOX_BAD, -1, aux );
			return;
		}
	}
	if (data->date) {
		/* configure ensures that %z actually works. */
		my_strftime( datestr, sizeof(datestr), "%d-%b-%Y %H:%M:%S %z", localtime( &data->date ) );
		imap_exec( ctx, &cmd->gen, imap_store_msg_p2,
		           "APPEND \"%\\s\" %s\"%\\s\" ", buf, flagstr, datestr );
	} else {
		imap_exec( ctx, &cmd->gen, imap_store_msg_p2,
		           "APPEND \"%\\s\" %s", buf, flagstr );
	}
	free( buf );
}

static void
imap_store_msg_p2( imap_store_t *ctx ATTR_UNUSED, imap_cmd_t *cmd, int response )
{
	imap_cmd_out_uid_t *cmdp = (imap_cmd_out_uid_t *)cmd;

	transform_msg_response( &response );
	cmdp->callback( response, cmdp->out_uid, cmdp->callback_aux );
}

/******************* imap_find_new_msgs *******************/

static void imap_find_new_msgs_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_find_new_msgs_p3( imap_store_t *, imap_cmd_t *, int );
static void imap_find_new_msgs_p4( imap_store_t *, imap_cmd_t *, int );

static void
imap_find_new_msgs( store_t *gctx, uint newuid,
                    void (*cb)( int sts, message_t *msgs, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;
	imap_cmd_find_new_t *cmd;

	INIT_IMAP_CMD(imap_cmd_find_new_t, cmd, cb, aux)
	cmd->out_msgs = ctx->msgapp;
	cmd->uid = newuid;
	// Some servers fail to enumerate recently STOREd messages without syncing first.
	imap_exec( (imap_store_t *)ctx, &cmd->gen, imap_find_new_msgs_p2, "CHECK" );
}

static void
imap_find_new_msgs_p2( imap_store_t *ctx, imap_cmd_t *gcmd, int response )
{
	imap_cmd_find_new_t *cmdp = (imap_cmd_find_new_t *)gcmd;
	imap_cmd_find_new_t *cmd;

	if (response != RESP_OK) {
		imap_done_simple_box( ctx, gcmd, response );
		return;
	}

	ctx->uidnext = 0;

	INIT_IMAP_CMD(imap_cmd_find_new_t, cmd, cmdp->callback, cmdp->callback_aux)
	cmd->out_msgs = cmdp->out_msgs;
	cmd->uid = cmdp->uid;
	cmd->gen.param.lastuid = 1;
	imap_exec( ctx, &cmd->gen, imap_find_new_msgs_p3,
	           "UID FETCH * (UID)" );
}

static void
imap_find_new_msgs_p3( imap_store_t *ctx, imap_cmd_t *gcmd, int response )
{
	imap_cmd_find_new_t *cmdp = (imap_cmd_find_new_t *)gcmd;
	imap_cmd_find_new_t *cmd;

	if (response != RESP_OK) {
		imap_find_new_msgs_p4( ctx, gcmd, response );
		return;
	}
	if (!ctx->uidnext) {
		// We are assuming that the new messages were not in fact instantly deleted.
		error( "IMAP error: re-querying server for highest UID failed\n" );
		imap_find_new_msgs_p4( ctx, gcmd, RESP_NO );
		return;
	}
	INIT_IMAP_CMD(imap_cmd_find_new_t, cmd, cmdp->callback, cmdp->callback_aux)
	cmd->out_msgs = cmdp->out_msgs;
	imap_exec( (imap_store_t *)ctx, &cmd->gen, imap_find_new_msgs_p4,
	           "UID FETCH %u:%u (UID BODY.PEEK[HEADER.FIELDS (X-TUID)])", cmdp->uid, ctx->uidnext - 1 );
}

static void
imap_find_new_msgs_p4( imap_store_t *ctx ATTR_UNUSED, imap_cmd_t *gcmd, int response )
{
	imap_cmd_find_new_t *cmdp = (imap_cmd_find_new_t *)gcmd;

	transform_box_response( &response );
	cmdp->callback( response, *cmdp->out_msgs, cmdp->callback_aux );
}

/******************* imap_list_store *******************/

typedef struct {
	imap_cmd_refcounted_state_t gen;
	void (*callback)( int sts, string_list_t *, void *aux );
	void *callback_aux;
} imap_list_store_state_t;

static void imap_list_store_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_list_store_p3( imap_store_t *, imap_list_store_state_t * );

static void
imap_list_store( store_t *gctx, int flags,
                 void (*cb)( int sts, string_list_t *boxes, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;
	INIT_REFCOUNTED_STATE(imap_list_store_state_t, sts, cb, aux)

	// ctx->prefix may be empty, "INBOX.", or something else.
	// 'flags' may be LIST_INBOX, LIST_PATH (or LIST_PATH_MAYBE), or both. 'listed'
	// already containing a particular value effectively removes it from 'flags'.
	// This matrix determines what to query, and what comes out as a side effect.
	// The lowercase letters indicate unnecessary results; the queries are done
	// this way to have non-overlapping result sets, so subsequent calls create
	// no duplicates:
	//
	// qry \ pfx | empty | inbox | other
	// ----------+-------+-------+-------
	// inbox     | p [I] | I [p] | I
	// both      | P [I] | I [P] | I + P
	// path      | P [i] | i [P] | P
	//
	int pfx_is_empty = !*ctx->prefix;
	int pfx_is_inbox = !pfx_is_empty && is_inbox( ctx, ctx->prefix, -1 );
	if (((flags & (LIST_PATH | LIST_PATH_MAYBE)) || pfx_is_empty) && !pfx_is_inbox && !(ctx->listed & LIST_PATH)) {
		ctx->listed |= LIST_PATH;
		if (pfx_is_empty)
			ctx->listed |= LIST_INBOX;
		imap_exec( ctx, imap_refcounted_new_cmd( &sts->gen ), imap_list_store_p2,
		           "LIST \"\" \"%\\s*\"", ctx->prefix );
	}
	if (((flags & LIST_INBOX) || pfx_is_inbox) && !pfx_is_empty && !(ctx->listed & LIST_INBOX)) {
		ctx->listed |= LIST_INBOX;
		if (pfx_is_inbox)
			ctx->listed |= LIST_PATH;
		imap_exec( ctx, imap_refcounted_new_cmd( &sts->gen ), imap_list_store_p2,
		           "LIST \"\" INBOX*" );
	}
	imap_list_store_p3( ctx, sts );
}

static void
imap_list_store_p2( imap_store_t *ctx, imap_cmd_t *cmd, int response )
{
	imap_list_store_state_t *sts = (imap_list_store_state_t *)((imap_cmd_refcounted_t *)cmd)->state;

	transform_refcounted_box_response( &sts->gen, response );
	imap_list_store_p3( ctx, sts );
}

static void
imap_list_store_p3( imap_store_t *ctx, imap_list_store_state_t *sts )
{
	DONE_REFCOUNTED_STATE_ARGS(sts, , ctx->boxes)
}

/******************* imap_cancel_cmds *******************/

static void
imap_cancel_cmds( store_t *gctx,
                  void (*cb)( void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	cancel_pending_imap_cmds( ctx );
	if (ctx->in_progress) {
		ctx->canceling = 1;
		ctx->callbacks.imap_cancel = cb;
		ctx->callback_aux = aux;
	} else {
		cb( aux );
	}
}

/******************* imap_commit_cmds *******************/

static void
imap_commit_cmds( store_t *gctx )
{
	(void)gctx;
}

/******************* imap_get_memory_usage *******************/

static int
imap_get_memory_usage( store_t *gctx )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	return ctx->buffer_mem + ctx->conn.buffer_mem;
}

/******************* imap_get_fail_state *******************/

static int
imap_get_fail_state( store_conf_t *gconf )
{
	return ((imap_store_conf_t *)gconf)->server->failed;
}

/******************* imap_parse_store *******************/

imap_server_conf_t *servers, **serverapp = &servers;

static int
imap_parse_store( conffile_t *cfg, store_conf_t **storep )
{
	imap_store_conf_t *store;
	imap_server_conf_t *server, *srv, sserver;
	const char *type, *name, *arg;
	unsigned u;
	int acc_opt = 0;
#ifdef HAVE_LIBSSL
	/* Legacy SSL options */
	int require_ssl = -1, use_imaps = -1;
	int use_sslv3 = -1, use_tlsv1 = -1, use_tlsv11 = -1, use_tlsv12 = -1;
#endif
	/* Legacy SASL option */
	int require_cram = -1;

	if (!strcasecmp( "IMAPAccount", cfg->cmd )) {
		server = nfcalloc( sizeof(*server) );
		server->name = nfstrdup( cfg->val );
		*serverapp = server;
		serverapp = &server->next;
		store = 0;
		*storep = 0;
	} else if (!strcasecmp( "IMAPStore", cfg->cmd )) {
		store = nfcalloc( sizeof(*store) );
		store->gen.driver = &imap_driver;
		store->gen.name = nfstrdup( cfg->val );
		store->use_namespace = 1;
		*storep = &store->gen;
		memset( &sserver, 0, sizeof(sserver) );
		server = &sserver;
	} else
		return 0;

	server->sconf.timeout = 20;
#ifdef HAVE_LIBSSL
	server->ssl_type = -1;
	server->sconf.ssl_versions = -1;
	server->sconf.system_certs = 1;
#endif
	server->max_in_progress = INT_MAX;

	while (getcline( cfg ) && cfg->cmd) {
		if (!strcasecmp( "Host", cfg->cmd )) {
			/* The imap[s]: syntax is just a backwards compat hack. */
			arg = cfg->val;
#ifdef HAVE_LIBSSL
			if (starts_with( arg, -1, "imaps:", 6 )) {
				arg += 6;
				server->ssl_type = SSL_IMAPS;
				if (server->sconf.ssl_versions == -1)
					server->sconf.ssl_versions = SSLv3 | TLSv1 | TLSv1_1 | TLSv1_2;
			} else
#endif
			if (starts_with( arg, -1, "imap:", 5 ))
				arg += 5;
			if (starts_with( arg, -1, "//", 2 ))
				arg += 2;
			if (arg != cfg->val)
				warn( "%s:%d: Notice: URL notation is deprecated; use a plain host name and possibly 'SSLType IMAPS' instead\n", cfg->file, cfg->line );
			server->sconf.host = nfstrdup( arg );
		}
		else if (!strcasecmp( "User", cfg->cmd ))
			server->user = nfstrdup( cfg->val );
		else if (!strcasecmp( "Pass", cfg->cmd ))
			server->pass = nfstrdup( cfg->val );
		else if (!strcasecmp( "PassCmd", cfg->cmd ))
			server->pass_cmd = nfstrdup( cfg->val );
		else if (!strcasecmp( "Port", cfg->cmd )) {
			int port = parse_int( cfg );
			if ((unsigned)port > 0xffff) {
				error( "%s:%d: Invalid port number\n", cfg->file, cfg->line );
				cfg->err = 1;
			} else {
				server->sconf.port = (ushort)port;
			}
		} else if (!strcasecmp( "Timeout", cfg->cmd ))
			server->sconf.timeout = parse_int( cfg );
		else if (!strcasecmp( "PipelineDepth", cfg->cmd )) {
			if ((server->max_in_progress = parse_int( cfg )) < 1) {
				error( "%s:%d: PipelineDepth must be at least 1\n", cfg->file, cfg->line );
				cfg->err = 1;
			}
		} else if (!strcasecmp( "DisableExtension", cfg->cmd ) ||
		           !strcasecmp( "DisableExtensions", cfg->cmd )) {
			arg = cfg->val;
			do {
				for (u = 0; u < as(cap_list); u++) {
					if (!strcasecmp( cap_list[u], arg )) {
						server->cap_mask |= 1 << u;
						goto gotcap;
					}
				}
				error( "%s:%d: Unrecognized IMAP extension '%s'\n", cfg->file, cfg->line, arg );
				cfg->err = 1;
			  gotcap: ;
			} while ((arg = get_arg( cfg, ARG_OPTIONAL, 0 )));
		}
#ifdef HAVE_LIBSSL
		else if (!strcasecmp( "CertificateFile", cfg->cmd )) {
			server->sconf.cert_file = expand_strdup( cfg->val );
			if (access( server->sconf.cert_file, R_OK )) {
				sys_error( "%s:%d: CertificateFile '%s'",
				           cfg->file, cfg->line, server->sconf.cert_file );
				cfg->err = 1;
			}
		} else if (!strcasecmp( "SystemCertificates", cfg->cmd )) {
			server->sconf.system_certs = parse_bool( cfg );
		} else if (!strcasecmp( "ClientCertificate", cfg->cmd )) {
			server->sconf.client_certfile = expand_strdup( cfg->val );
			if (access( server->sconf.client_certfile, R_OK )) {
				sys_error( "%s:%d: ClientCertificate '%s'",
				           cfg->file, cfg->line, server->sconf.client_certfile );
				cfg->err = 1;
			}
		} else if (!strcasecmp( "ClientKey", cfg->cmd )) {
			server->sconf.client_keyfile = expand_strdup( cfg->val );
			if (access( server->sconf.client_keyfile, R_OK )) {
				sys_error( "%s:%d: ClientKey '%s'",
				           cfg->file, cfg->line, server->sconf.client_keyfile );
				cfg->err = 1;
			}
		} else if (!strcasecmp( "SSLType", cfg->cmd )) {
			if (!strcasecmp( "None", cfg->val )) {
				server->ssl_type = SSL_None;
			} else if (!strcasecmp( "STARTTLS", cfg->val )) {
				server->ssl_type = SSL_STARTTLS;
			} else if (!strcasecmp( "IMAPS", cfg->val )) {
				server->ssl_type = SSL_IMAPS;
			} else {
				error( "%s:%d: Invalid SSL type\n", cfg->file, cfg->line );
				cfg->err = 1;
			}
		} else if (!strcasecmp( "SSLVersion", cfg->cmd ) ||
		           !strcasecmp( "SSLVersions", cfg->cmd )) {
			server->sconf.ssl_versions = 0;
			arg = cfg->val;
			do {
				if (!strcasecmp( "SSLv2", arg )) {
					warn( "Warning: SSLVersion SSLv2 is no longer supported\n" );
				} else if (!strcasecmp( "SSLv3", arg )) {
					server->sconf.ssl_versions |= SSLv3;
				} else if (!strcasecmp( "TLSv1", arg )) {
					server->sconf.ssl_versions |= TLSv1;
				} else if (!strcasecmp( "TLSv1.1", arg )) {
					server->sconf.ssl_versions |= TLSv1_1;
				} else if (!strcasecmp( "TLSv1.2", arg )) {
					server->sconf.ssl_versions |= TLSv1_2;
				} else {
					error( "%s:%d: Unrecognized SSL version\n", cfg->file, cfg->line );
					cfg->err = 1;
				}
			} while ((arg = get_arg( cfg, ARG_OPTIONAL, 0 )));
		} else if (!strcasecmp( "RequireSSL", cfg->cmd ))
			require_ssl = parse_bool( cfg );
		else if (!strcasecmp( "UseIMAPS", cfg->cmd ))
			use_imaps = parse_bool( cfg );
		else if (!strcasecmp( "UseSSLv2", cfg->cmd ))
			warn( "Warning: UseSSLv2 is no longer supported\n" );
		else if (!strcasecmp( "UseSSLv3", cfg->cmd ))
			use_sslv3 = parse_bool( cfg );
		else if (!strcasecmp( "UseTLSv1", cfg->cmd ))
			use_tlsv1 = parse_bool( cfg );
		else if (!strcasecmp( "UseTLSv1.1", cfg->cmd ))
			use_tlsv11 = parse_bool( cfg );
		else if (!strcasecmp( "UseTLSv1.2", cfg->cmd ))
			use_tlsv12 = parse_bool( cfg );
#endif
		else if (!strcasecmp( "AuthMech", cfg->cmd ) ||
		         !strcasecmp( "AuthMechs", cfg->cmd )) {
			arg = cfg->val;
			do
				add_string_list( &server->auth_mechs, arg );
			while ((arg = get_arg( cfg, ARG_OPTIONAL, 0 )));
		} else if (!strcasecmp( "RequireCRAM", cfg->cmd ))
			require_cram = parse_bool( cfg );
		else if (!strcasecmp( "Tunnel", cfg->cmd ))
			server->sconf.tunnel = nfstrdup( cfg->val );
		else if (store) {
			if (!strcasecmp( "Account", cfg->cmd )) {
				for (srv = servers; srv; srv = srv->next)
					if (srv->name && !strcmp( srv->name, cfg->val ))
						goto gotsrv;
				error( "%s:%d: unknown IMAP account '%s'\n", cfg->file, cfg->line, cfg->val );
				cfg->err = 1;
				continue;
			  gotsrv:
				store->server = srv;
			} else if (!strcasecmp( "UseNamespace", cfg->cmd ))
				store->use_namespace = parse_bool( cfg );
			else if (!strcasecmp( "Path", cfg->cmd ))
				store->gen.path = nfstrdup( cfg->val );
			else if (!strcasecmp( "PathDelimiter", cfg->cmd )) {
				if (strlen( cfg->val ) != 1) {
					error( "%s:%d: Path delimiter must be exactly one character long\n", cfg->file, cfg->line );
					cfg->err = 1;
					continue;
				}
				store->delimiter = cfg->val[0];
			} else
				parse_generic_store( &store->gen, cfg );
			continue;
		} else {
			error( "%s:%d: unknown/misplaced keyword '%s'\n", cfg->file, cfg->line, cfg->cmd );
			cfg->err = 1;
			continue;
		}
		acc_opt = 1;
	}
	if (store)
		type = "IMAP store", name = store->gen.name;
	else
		type = "IMAP account", name = server->name;
	if (!store || !store->server) {
		if (!server->sconf.tunnel && !server->sconf.host) {
			error( "%s '%s' has neither Tunnel nor Host\n", type, name );
			cfg->err = 1;
			return 1;
		}
		if (server->pass && server->pass_cmd) {
			error( "%s '%s' has both Pass and PassCmd\n", type, name );
			cfg->err = 1;
			return 1;
		}
#ifdef HAVE_LIBSSL
		if ((use_sslv3 & use_tlsv1 & use_tlsv11 & use_tlsv12) != -1 || use_imaps >= 0 || require_ssl >= 0) {
			if (server->ssl_type >= 0 || server->sconf.ssl_versions >= 0) {
				error( "%s '%s': The deprecated UseSSL*, UseTLS*, UseIMAPS, and RequireSSL options are mutually exclusive with SSLType and SSLVersions.\n", type, name );
				cfg->err = 1;
				return 1;
			}
			warn( "Notice: %s '%s': UseSSL*, UseTLS*, UseIMAPS, and RequireSSL are deprecated. Use SSLType and SSLVersions instead.\n", type, name );
			server->sconf.ssl_versions =
					(use_sslv3 != 1 ? 0 : SSLv3) |
					(use_tlsv1 == 0 ? 0 : TLSv1) |
					(use_tlsv11 != 1 ? 0 : TLSv1_1) |
					(use_tlsv12 != 1 ? 0 : TLSv1_2);
			if (use_imaps == 1) {
				server->ssl_type = SSL_IMAPS;
			} else if (require_ssl) {
				server->ssl_type = SSL_STARTTLS;
			} else if (!server->sconf.ssl_versions) {
				server->ssl_type = SSL_None;
			} else {
				warn( "Notice: %s '%s': 'RequireSSL no' is being ignored\n", type, name );
				server->ssl_type = SSL_STARTTLS;
			}
			if (server->ssl_type != SSL_None && !server->sconf.ssl_versions) {
				error( "%s '%s' requires SSL but no SSL versions enabled\n", type, name );
				cfg->err = 1;
				return 1;
			}
		} else {
			if (server->sconf.ssl_versions < 0)
				server->sconf.ssl_versions = TLSv1 | TLSv1_1 | TLSv1_2;
			if (server->ssl_type < 0)
				server->ssl_type = server->sconf.tunnel ? SSL_None : SSL_STARTTLS;
		}
#endif
		if (require_cram >= 0) {
			if (server->auth_mechs) {
				error( "%s '%s': The deprecated RequireCRAM option is mutually exclusive with AuthMech.\n", type, name );
				cfg->err = 1;
				return 1;
			}
			warn( "Notice: %s '%s': RequireCRAM is deprecated. Use AuthMech instead.\n", type, name );
			if (require_cram)
				add_string_list(&server->auth_mechs, "CRAM-MD5");
		}
		if (!server->auth_mechs)
			add_string_list( &server->auth_mechs, "*" );
		if (!server->sconf.port)
			server->sconf.port =
#ifdef HAVE_LIBSSL
				server->ssl_type == SSL_IMAPS ? 993 :
#endif
				143;
	}
	if (store) {
		if (!store->server) {
			store->server = nfmalloc( sizeof(sserver) );
			memcpy( store->server, &sserver, sizeof(sserver) );
			store->server->name = store->gen.name;
		} else if (acc_opt) {
			error( "%s '%s' has both Account and account-specific options\n", type, name );
			cfg->err = 1;
		}
	}
	return 1;
}

static int
imap_get_caps( store_t *gctx ATTR_UNUSED )
{
	return DRV_CRLF | DRV_VERBOSE;
}

struct driver imap_driver = {
	imap_get_caps,
	imap_parse_store,
	imap_cleanup,
	imap_alloc_store,
	imap_set_bad_callback,
	imap_connect_store,
	imap_free_store,
	imap_cancel_store,
	imap_list_store,
	imap_select_box,
	imap_get_box_path,
	imap_create_box,
	imap_open_box,
	imap_get_uidnext,
	imap_confirm_box_empty,
	imap_delete_box,
	imap_finish_delete_box,
	imap_prepare_load_box,
	imap_load_box,
	imap_fetch_msg,
	imap_store_msg,
	imap_find_new_msgs,
	imap_set_msg_flags,
	imap_trash_msg,
	imap_close_box,
	imap_cancel_cmds,
	imap_commit_cmds,
	imap_get_memory_usage,
	imap_get_fail_state,
};
