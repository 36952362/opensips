/*
 * Copyright (C) 2011 OpenSIPS Solutions
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *
 * history:
 * ---------
 *  2011-09-xx  created (vlad-paiu)
 */

#include "../../dprint.h"
#include "cachedb_redis_dbase.h"
#include "cachedb_redis_utils.h"
#include "../../mem/mem.h"
#include "../../ut.h"
#include "../../pt.h"
#include "../../cachedb/cachedb.h"
#include "../../lib/csv.h"

#include <string.h>
#include <hiredis/hiredis.h>

#define QUERY_ATTEMPTS 2
#define REDIS_DF_PORT  6379

int redis_query_tout = CACHEDB_REDIS_DEFAULT_TIMEOUT;
int redis_connnection_tout = CACHEDB_REDIS_DEFAULT_TIMEOUT;
int shutdown_on_error = 0;
int use_tls = 0;

struct tls_mgm_binds tls_api;

redisContext *redis_get_ctx(char *ip, int port)
{
	struct timeval tv;
	static char warned = 0;
	redisContext *ctx;

	if (!port)
		port = REDIS_DF_PORT;

	if (!redis_connnection_tout) {
		if (!warned++)
			LM_WARN("Connecting to redis without timeout might block your server\n");
		ctx = redisConnect(ip,port);
	} else {
		tv.tv_sec = redis_connnection_tout / 1000;
		tv.tv_usec = (redis_connnection_tout * 1000) % 1000000;
		ctx = redisConnectWithTimeout(ip,port,tv);
	}
	if (ctx && ctx->err != REDIS_OK) {
		LM_ERR("failed to open redis connection %s:%hu - %s\n",ip,
				(unsigned short)port,ctx->errstr);
		return NULL;
	}

	if (redis_query_tout) {
		tv.tv_sec = redis_query_tout / 1000;
		tv.tv_usec = (redis_query_tout * 1000) % 1000000;
		if (redisSetTimeout(ctx, tv) != REDIS_OK) {
			LM_ERR("Cannot set query timeout to %dms\n", redis_query_tout);
			return NULL;
		}
	}
	return ctx;
}

#ifdef HAVE_REDIS_SSL
static void tls_print_errstack(void)
{
	int code;

	while ((code = ERR_get_error())) {
		LM_ERR("TLS errstack: %s\n", ERR_error_string(code, 0));
	}
}

static int redis_init_ssl(char *url_extra_opts, redisContext *ctx,
	struct tls_domain **tls_dom)
{
	str tls_dom_name;
	SSL *ssl;
	struct tls_domain *d;

	if (tls_dom == NULL) {
		if (strncmp(url_extra_opts, CACHEDB_TLS_DOM_PARAM,
				CACHEDB_TLS_DOM_PARAM_LEN)) {
			LM_ERR("Invalid Redis URL parameter: %s\n", url_extra_opts);
			return -1;
		}

		tls_dom_name.s = url_extra_opts + CACHEDB_TLS_DOM_PARAM_LEN;
		tls_dom_name.len = strlen(tls_dom_name.s);
		if (!tls_dom_name.len) {
			LM_ERR("Empty TLS domain name in Redis URL\n");
			return -1;
		}

		d = tls_api.find_client_domain_name(&tls_dom_name);
		if (d == NULL) {
			LM_ERR("TLS domain: %.*s not found\n",
				tls_dom_name.len, tls_dom_name.s);
			return -1;
		}

		*tls_dom = d;
	} else {
		d = *tls_dom;
	}

	ssl = SSL_new(((void**)d->ctx)[process_no]);
	if (!ssl) {
		LM_ERR("failed to create SSL structure (%d:%s)\n", errno, strerror(errno));
		tls_print_errstack();
		tls_api.release_domain(*tls_dom);
		return -1;
	}

	if (redisInitiateSSL(ctx, ssl) != REDIS_OK) {
		printf("Failed to init Redis SSL: %s\n", ctx->errstr);
		tls_api.release_domain(*tls_dom);
		return -1;
	}

	LM_DBG("TLS enabled for this connection\n");

	return 0;
}
#endif

int redis_connect_node(redis_con *con,cluster_node *node)
{
	redisReply *rpl;

	node->context = redis_get_ctx(node->ip,node->port);
	if (!node->context)
		return -1;

#ifdef HAVE_REDIS_SSL
	if (use_tls && con->id->extra_options &&
		redis_init_ssl(con->id->extra_options, node->context,
			&node->tls_dom) < 0) {
		redisFree(node->context);
		return -1;
	}
#endif

	if (con->id->password) {
		rpl = redisCommand(node->context,"AUTH %s",con->id->password);
		if (rpl == NULL || rpl->type == REDIS_REPLY_ERROR) {
			LM_ERR("failed to auth to redis - %.*s\n",
				rpl?(unsigned)rpl->len:7,rpl?rpl->str:"FAILURE");
			freeReplyObject(rpl);
			goto error;
		}
		LM_DBG("AUTH [password] -  %.*s\n",(unsigned)rpl->len,rpl->str);
		freeReplyObject(rpl);
	}

	if ((con->flags & REDIS_SINGLE_INSTANCE) && con->id->database) {
		rpl = redisCommand(node->context,"SELECT %s",con->id->database);
		if (rpl == NULL || rpl->type == REDIS_REPLY_ERROR) {
			LM_ERR("failed to select database %s - %.*s\n",con->id->database,
				rpl?(unsigned)rpl->len:7,rpl?rpl->str:"FAILURE");
			freeReplyObject(rpl);
			goto error;
		}

		LM_DBG("SELECT [%s] - %.*s\n",con->id->database,(unsigned)rpl->len,rpl->str);
		freeReplyObject(rpl);
	}

	return 0;

error:
	redisFree(node->context);
	if (use_tls && node->tls_dom) {
		tls_api.release_domain(node->tls_dom);
		node->tls_dom = NULL;
	}
	return -1;
}

int redis_reconnect_node(redis_con *con,cluster_node *node)
{
	LM_DBG("reconnecting node %s:%d \n",node->ip,node->port);

	/* close the old connection */
	if(node->context)
		redisFree(node->context);

	return redis_connect_node(con,node);
}

int redis_connect(redis_con *con)
{
	redisContext *ctx;
	redisReply *rpl;
	cluster_node *it;
	int len;
	struct tls_domain *tls_dom = NULL;

	/* connect to redis DB */
	ctx = redis_get_ctx(con->host,con->port);
	if (!ctx)
		return -1;

#ifdef HAVE_REDIS_SSL
	if (use_tls && con->id->extra_options &&
		redis_init_ssl(con->id->extra_options, ctx, &tls_dom) < 0) {
		redisFree(ctx);
		return -1;
	}
#endif

	/* auth using password, if any */
	if (con->id->password) {
		rpl = redisCommand(ctx,"AUTH %s",con->id->password);
		if (rpl == NULL || rpl->type == REDIS_REPLY_ERROR) {
			LM_ERR("failed to auth to redis - %.*s\n",
				rpl?(unsigned)rpl->len:7,rpl?rpl->str:"FAILURE");
			if (rpl!=NULL)
				freeReplyObject(rpl);
			goto error;
		}
		LM_DBG("AUTH [password] -  %.*s\n",(unsigned)rpl->len,rpl->str);
		freeReplyObject(rpl);
	}

	rpl = redisCommand(ctx,"CLUSTER NODES");
	if (rpl == NULL || rpl->type == REDIS_REPLY_ERROR) {
		/* single instace mode */
		con->flags |= REDIS_SINGLE_INSTANCE;
		len = strlen(con->host);
		con->nodes = pkg_malloc(sizeof(cluster_node) + len + 1);
		if (con->nodes == NULL) {
			LM_ERR("no more pkg\n");
			if (rpl!=NULL)
				freeReplyObject(rpl);
			goto error;
		}
		con->nodes->ip = (char *)(con->nodes + 1);

		strcpy(con->nodes->ip,con->host);
		con->nodes->port = con->port;
		con->nodes->start_slot = 0;
		con->nodes->end_slot = 4096;
		con->nodes->context = NULL;
		con->nodes->next = NULL;
		LM_DBG("single instance mode\n");
	} else {
		/* cluster instance mode */
		con->flags |= REDIS_CLUSTER_INSTANCE;
		con->slots_assigned = 0;
		LM_DBG("cluster instance mode\n");
		if (build_cluster_nodes(con,rpl->str,rpl->len) < 0) {
			LM_ERR("failed to parse Redis cluster info\n");
			freeReplyObject(rpl);
			goto error;
		}
	}

	if (rpl!=NULL)
		freeReplyObject(rpl);
	redisFree(ctx);

	if (use_tls && tls_dom)
		tls_api.release_domain(tls_dom);

	con->flags |= REDIS_INIT_NODES;

	for (it=con->nodes;it;it=it->next) {

		if (it->end_slot > con->slots_assigned )
			con->slots_assigned = it->end_slot;

		if (redis_connect_node(con,it) < 0) {
			LM_ERR("failed to init connection \n");
			return -1;
		}
	}

	return 0;

error:
	redisFree(ctx);
	if (use_tls && tls_dom)
		tls_api.release_domain(tls_dom);
	return -1;
}

/* free a circular list of Redis connections */
void redis_free_conns(redis_con *con)
{
	redis_con *aux = NULL, *head = con;

	while (con && (con != head || !aux)) {
		aux = con;
		con = con->next_con;
		pkg_free(aux->host);
		pkg_free(aux);
	}
}

/* parse a string of: "host[:port]" */
int redis_get_hostport(const str *hostport, char **host, unsigned short *port)
{
	str in, out;

	char *p = q_memchr(hostport->s, ':', hostport->len);
	if (!p) {
		if (pkg_nt_str_dup(&out, hostport) != 0) {
			LM_ERR("oom\n");
			return -1;
		}

		*host = out.s;
		*port = REDIS_DF_PORT;
	} else {
		in.s = hostport->s;
		in.len = p - hostport->s;
		if (pkg_nt_str_dup(&out, &in) != 0) {
			LM_ERR("oom\n");
			return -1;
		}
		*host = out.s;

		in.s = p + 1;
		in.len = hostport->s + hostport->len - (p + 1);
		if (in.len <= 0) {
			LM_ERR("bad/missing Redis port in URL\n");
			return -1;
		}

		unsigned int out_port;
		if (str2int(&in, &out_port) != 0) {
			LM_ERR("failed to parse Redis port in URL\n");
			return -1;
		}

		*port = out_port;
	}

	LM_DBG("extracted from '%.*s': '%s' and %d\n", hostport->len, hostport->s,
	       *host, *port);

	return 0;
}

redis_con* redis_new_connection(struct cachedb_id* id)
{
	redis_con *con, *cons = NULL;
	csv_record *r, *it;
	unsigned int multi_hosts;

	if (id == NULL) {
		LM_ERR("null cachedb_id\n");
		return NULL;
	}

	if (id->flags & CACHEDB_ID_MULTIPLE_HOSTS)
		multi_hosts = REDIS_MULTIPLE_HOSTS;
	else
		multi_hosts = 0;

	r = parse_csv_record(_str(id->host));
	for (it = r; it; it = it->next) {
		LM_DBG("parsed Redis host: '%.*s'\n", it->s.len, it->s.s);

		con = pkg_malloc(sizeof(redis_con));
		if (con == NULL) {
			LM_ERR("no more pkg\n");
			goto out_err;
		}

		memset(con,0,sizeof(redis_con));

		if (redis_get_hostport(&it->s, &con->host, &con->port) != 0) {
			LM_ERR("no more pkg\n");
			goto out_err;
		}

		con->id = id;
		con->ref = 1;
		con->flags |= multi_hosts; /* if the case */

		/* if doing failover Redises, only connect the 1st one for now! */
		if (!cons && redis_connect(con) < 0) {
			LM_ERR("failed to connect to DB\n");
			if (shutdown_on_error)
				goto out_err;
		}

		_add_last(con, cons, next_con);
	}

	/* turn @cons into a circular list */
	con->next_con = cons;
	/* set the "last-known-to-work" connection */
	cons->current = cons;

	free_csv_record(r);
	return cons;

out_err:
	free_csv_record(r);
	redis_free_conns(cons);
	return NULL;
}

cachedb_con *redis_init(str *url)
{
	return cachedb_do_init(url,(void *)redis_new_connection);
}

void redis_free_connection(cachedb_pool_con *cpc)
{
	redis_con *con = (redis_con *)cpc, *aux = NULL, *head = con;

	LM_DBG("in redis_free_connection\n");

	if (!con)
		return;

	while (con && (con != head || !aux)) {
		aux = con;
		con = con->next_con;
		destroy_cluster_nodes(aux);
		pkg_free(aux->host);
		pkg_free(aux);
	}
}

void redis_destroy(cachedb_con *con) {
	LM_DBG("in redis_destroy\n");
	cachedb_do_close(con,redis_free_connection);
}

/*
 * Upon returning 0 (success), @rpl is guaranteed to be:
 *   - non-NULL
 *   - non-REDIS_REPLY_ERROR
 *
 * On error, a negative code is returned
 */
static int redis_run_command(cachedb_con *connection, redisReply **rpl,
              str *key, char *cmd_fmt, ...)
{
	redis_con *con = NULL, *first;
	cluster_node *node;
	redisReply *reply = NULL;
	va_list ap;
	int i, last_err = 0;

	first = ((redis_con *)connection->data)->current;
	while (((redis_con *)connection->data)->current != first || !con) {
		con = ((redis_con *)connection->data)->current;

		if (!(con->flags & REDIS_INIT_NODES) && redis_connect(con) < 0) {
			LM_ERR("failed to connect to DB\n");
			last_err = -9;
			goto try_next_con;
		}

		node = get_redis_connection(con,key);
		if (node == NULL) {
			LM_ERR("Bad cluster configuration\n");
			last_err = -10;
			goto try_next_con;
		}

		if (node->context == NULL) {
			if (redis_reconnect_node(con,node) < 0) {
				last_err = -1;
				goto try_next_con;
			}
		}

		va_start(ap, cmd_fmt);

		for (i = QUERY_ATTEMPTS; i; i--) {
			reply = redisvCommand(node->context, cmd_fmt, ap);
			if (reply == NULL || reply->type == REDIS_REPLY_ERROR) {
				LM_INFO("Redis query failed: %p %.*s (%s)\n",
					reply,reply?(unsigned)reply->len:7,reply?reply->str:"FAILURE",
					node->context->errstr);
				if (reply) {
					freeReplyObject(reply);
					reply = NULL;
				}
				if (node->context->err == REDIS_OK || redis_reconnect_node(con,node) < 0) {
					i = 0; break;
				}
			} else break;
		}

		va_end(ap);

		if (i==0) {
			LM_ERR("giving up on query to %s:%d\n", con->host, con->port);
			last_err = -1;
			goto try_next_con;
		}

		if (i != QUERY_ATTEMPTS)
			LM_INFO("successfully ran query after %d failed attempt(s)\n",
			        QUERY_ATTEMPTS - i);

		last_err = 0;
		break;

try_next_con:
		((redis_con *)connection->data)->current = con->next_con;
		if (con->next_con != first)
			LM_INFO("failing over to next Redis host (%s:%d)\n",
			        con->next_con->host, con->next_con->port);
	}

	*rpl = reply;
	return last_err;
}

int redis_get(cachedb_con *connection,str *attr,str *val)
{
	redisReply *reply;
	int rc;

	if (!attr || !val || !connection) {
		LM_ERR("null parameter\n");
		return -1;
	}

	rc = redis_run_command(connection, &reply, attr, "GET %b", attr->s, attr->len);
	if (rc != 0)
		goto out_err;

	if (reply->type == REDIS_REPLY_NIL) {
		LM_DBG("no such key - %.*s\n",attr->len,attr->s);
		val->s = NULL;
		val->len = 0;
		freeReplyObject(reply);
		return -2;
	}

	if (reply->str == NULL || reply->len == 0) {
		/* empty string key */
		val->s = NULL;
		val->len = 0;
		freeReplyObject(reply);
		return 0;
	}

	LM_DBG("GET %.*s  - %.*s\n",attr->len,attr->s,(unsigned)reply->len,reply->str);

	val->s = pkg_malloc(reply->len);
	if (val->s == NULL) {
		LM_ERR("no more pkg\n");
		goto out_err;
	}

	memcpy(val->s,reply->str,reply->len);
	val->len = reply->len;
	freeReplyObject(reply);
	return 0;

out_err:
	if (reply)
		freeReplyObject(reply);
	return rc;
}

int redis_set(cachedb_con *connection,str *attr,str *val,int expires)
{
	redisReply *reply;
	int rc;

	if (!attr || !val || !connection) {
		LM_ERR("null parameter\n");
		return -1;
	}

	rc = redis_run_command(connection, &reply, attr, "SET %b %b",
			attr->s, (size_t)attr->len, val->s, (size_t)val->len);
	if (rc != 0)
		goto out_err;

	LM_DBG("set %.*s to %.*s - status = %d - %.*s\n",attr->len,attr->s,val->len,
			val->s,reply->type,(unsigned)reply->len,reply->str);

	freeReplyObject(reply);

	if (expires) {
		rc = redis_run_command(connection, &reply, attr, "EXPIRE %b %d",
		             attr->s, (size_t)attr->len, expires);
		if (rc != 0)
			goto out_err;

		LM_DBG("set %.*s to expire in %d s - %.*s\n",attr->len,attr->s,expires,
				(unsigned)reply->len,reply->str);

		freeReplyObject(reply);
	}

	return 0;

out_err:
	freeReplyObject(reply);
	return rc;
}

/* returns 0 in case of successful remove
 * returns 1 in case of key not existent
 * return -1 in case of error */
int redis_remove(cachedb_con *connection,str *attr)
{
	redisReply *reply;
	int rc;

	if (!attr || !connection) {
		LM_ERR("null parameter\n");
		return -1;
	}

	rc = redis_run_command(connection, &reply, attr, "DEL %b",
		attr->s, (size_t)attr->len);
	if (rc != 0)
		goto out_err;

	if (reply->integer == 0) {
		LM_DBG("Key %.*s does not exist in DB\n",attr->len,attr->s);
		rc = 1;
	} else
		LM_DBG("Key %.*s successfully removed\n",attr->len,attr->s);

	freeReplyObject(reply);
	return rc;

out_err:
	freeReplyObject(reply);
	return rc;
}

/* returns the new value of the counter */
int redis_add(cachedb_con *connection,str *attr,int val,int expires,int *new_val)
{
	redisReply *reply;
	int rc;

	if (!attr || !connection) {
		LM_ERR("null parameter\n");
		return -1;
	}

	rc = redis_run_command(connection, &reply, attr, "INCRBY %b %d",
			attr->s, (size_t)attr->len,val);
	if (rc != 0)
		goto out_err;

	if (new_val)
		*new_val = reply->integer;
	freeReplyObject(reply);

	if (expires) {
		rc = redis_run_command(connection, &reply, attr, "EXPIRE %b %d",
				attr->s, (size_t)attr->len,expires);
		if (rc != 0)
			goto out_err;

		LM_DBG("set %.*s to expire in %d s - %.*s\n",attr->len,attr->s,expires,
				(unsigned)reply->len,reply->str);

		freeReplyObject(reply);
	}

	return rc;

out_err:
	freeReplyObject(reply);
	return rc;
}

int redis_sub(cachedb_con *connection,str *attr,int val,int expires,int *new_val)
{
	redisReply *reply;
	int rc;

	if (!attr || !connection) {
		LM_ERR("null parameter\n");
		return -1;
	}

	rc = redis_run_command(connection, &reply, attr, "DECRBY %b %d",
			attr->s, (size_t)attr->len, val);
	if (rc != 0)
		goto out_err;

	if (new_val)
		*new_val = reply->integer;
	freeReplyObject(reply);

	if (expires) {
		rc = redis_run_command(connection, &reply, attr, "EXPIRE %b %d",
				attr->s, (size_t)attr->len, expires);
		if (rc != 0)
			goto out_err;

		LM_DBG("set %.*s to expire in %d s - %.*s\n",attr->len,attr->s,expires,
				(unsigned)reply->len,reply->str);

		freeReplyObject(reply);
	}

	return 0;

out_err:
	freeReplyObject(reply);
	return rc;
}

int redis_get_counter(cachedb_con *connection,str *attr,int *val)
{
	redisReply *reply;
	int ret, rc;
	str response;

	if (!attr || !val || !connection) {
		LM_ERR("null parameter\n");
		return -1;
	}

	rc = redis_run_command(connection, &reply, attr, "GET %b",
			attr->s, (size_t)attr->len);
	if (rc != 0)
		goto out_err;

	if (reply->type == REDIS_REPLY_NIL || reply->str == NULL
			|| reply->len == 0) {
		LM_DBG("no such key - %.*s\n",attr->len,attr->s);
		return -2;
	}

	LM_DBG("GET %.*s  - %.*s\n",attr->len,attr->s,(unsigned)reply->len,reply->str);

	response.s=reply->str;
	response.len=reply->len;

	if (str2sint(&response,&ret) != 0) {
		LM_ERR("Not a counter \n");
		freeReplyObject(reply);
		return -3;
	}

	if (val)
		*val = ret;

	freeReplyObject(reply);
	return 0;

out_err:
	freeReplyObject(reply);
	return rc;
}

int redis_raw_query_handle_reply(redisReply *reply,cdb_raw_entry ***ret,
		int expected_kv_no,int *reply_no)
{
	int current_size=0,len,i;

	/* start with a single returned document */
	*ret = pkg_malloc(1 * sizeof(cdb_raw_entry *));
	if (*ret == NULL) {
		LM_ERR("No more PKG mem\n");
		goto error;
	}

	**ret = pkg_malloc(expected_kv_no * sizeof(cdb_raw_entry));
	if (**ret == NULL) {
		LM_ERR("No more pkg mem\n");
		goto error;
	}

	switch (reply->type) {
		case REDIS_REPLY_STRING:
			(*ret)[current_size][0].val.s.s = pkg_malloc(reply->len);
			if (! (*ret)[current_size][0].val.s.s ) {
				LM_ERR("No more pkg \n");
				goto error;
			}

			memcpy((*ret)[current_size][0].val.s.s,reply->str,reply->len);
			(*ret)[current_size][0].val.s.len = reply->len;
			(*ret)[current_size][0].type = CDB_STR;

			current_size++;
			break;
		case REDIS_REPLY_INTEGER:
			(*ret)[current_size][0].val.n = reply->integer;
			(*ret)[current_size][0].type = CDB_INT32;
			current_size++;
			break;
		case REDIS_REPLY_NIL:
			(*ret)[current_size][0].type = CDB_NULL;
			(*ret)[current_size][0].val.s.s = NULL;
			(*ret)[current_size][0].val.s.len = 0;
			current_size++;
			break;
		case REDIS_REPLY_ARRAY:
			for (i=0;i<reply->elements;i++) {
				switch (reply->element[i]->type) {
					case REDIS_REPLY_STRING:
					case REDIS_REPLY_INTEGER:
					case REDIS_REPLY_NIL:
						if (current_size > 0) {
							*ret = pkg_realloc(*ret,(current_size + 1) * sizeof(cdb_raw_entry *));
							if (*ret == NULL) {
								LM_ERR("No more pkg\n");
								goto error;
							}
							(*ret)[current_size] = pkg_malloc(expected_kv_no * sizeof(cdb_raw_entry));
							if ((*ret)[current_size] == NULL) {
								LM_ERR("No more pkg\n");
								goto error;
							}
						}


						if (reply->element[i]->type == REDIS_REPLY_INTEGER) {
							(*ret)[current_size][0].val.n = reply->element[i]->integer;
							(*ret)[current_size][0].type = CDB_INT32;
						} else if (reply->element[i]->type == REDIS_REPLY_NIL) {
							(*ret)[current_size][0].val.s.s = NULL;
							(*ret)[current_size][0].val.s.len = 0;
							(*ret)[current_size][0].type = CDB_NULL;
						} else {
							(*ret)[current_size][0].val.s.s = pkg_malloc(reply->element[i]->len);
							if (! (*ret)[current_size][0].val.s.s ) {
								pkg_free((*ret)[current_size]);
								LM_ERR("No more pkg \n");
								goto error;
							}

							memcpy((*ret)[current_size][0].val.s.s,reply->element[i]->str,reply->element[i]->len);
							(*ret)[current_size][0].val.s.len = reply->element[i]->len;
							(*ret)[current_size][0].type = CDB_STR;
						}

						current_size++;
						break;
					default:
						LM_DBG("Unexpected data type %d found in array - skipping \n",reply->element[i]->type);
				}
			}
			break;
		default:
			LM_ERR("unhandled Redis datatype %d\n", reply->type);
			goto error;
	}

	if (current_size == 0)
		pkg_free((*ret)[0]);

	*reply_no = current_size;
	freeReplyObject(reply);
	return 1;

error:
	if (current_size == 0 && *ret)
		pkg_free((*ret)[0]);

	if (*ret) {
		for (len = 0;len<current_size;len++) {
			if ( (*ret)[len][0].type == CDB_STR)
				pkg_free((*ret)[len][0].val.s.s);
			pkg_free((*ret)[len]);
		}
		pkg_free(*ret);
	}

	*ret = NULL;
	*reply_no=0;

	freeReplyObject(reply);
	return -1;
}

/* TODO - altough in most of the cases the targetted key is the 2nd query string,
	that's not always the case ! - make this 100% */
int redis_raw_query_extract_key(str *attr,str *query_key)
{
	int len;
	char *p,*q,*r;

	if (!attr || attr->s == NULL || query_key == NULL)
		return -1;

	trim_len(len,p,*attr);
	q = memchr(p,' ',len);
	if (q == NULL) {
		LM_ERR("Malformed Redis RAW query \n");
		return -1;
	}

	query_key->s = q+1;
	r = memchr(query_key->s,' ',len - (query_key->s - p));
	if (r == NULL) {
		query_key->len = (p+len) - query_key->s;
	} else {
		query_key->len = r-query_key->s;
	}

	return 0;
}

int redis_raw_query_send(cachedb_con *connection, redisReply **reply,
		cdb_raw_entry ***_, int __, int *___, str *attr, ...)
{
	static str attr_nt;
	str query_key;

	if (redis_raw_query_extract_key(attr, &query_key) < 0) {
		LM_ERR("Failed to extract Redis raw query key\n");
		return -1;
	}

	if (pkg_str_extend(&attr_nt, attr->len + 1) < 0) {
		LM_ERR("oom\n");
		return -1;
	}

	memcpy(attr_nt.s, attr->s, attr->len);
	attr_nt.s[attr->len] = '\0';

	return redis_run_command(connection, reply, &query_key, attr_nt.s);
}

int redis_raw_query(cachedb_con *connection,str *attr,cdb_raw_entry ***rpl,int expected_kv_no,int *reply_no)
{
	redisReply *reply;

	if (!attr || !connection) {
		LM_ERR("null parameter\n");
		return -1;
	}


	if (redis_raw_query_send(connection,&reply,rpl,expected_kv_no,reply_no,attr) < 0) {
		LM_ERR("Failed to send query to server \n");
		return -1;
	}

	switch (reply->type) {
		case REDIS_REPLY_ERROR:
			LM_ERR("Error encountered when running Redis raw query [%.*s]\n",
			attr->len,attr->s);
			return -1;
		case REDIS_REPLY_NIL:
			LM_DBG("Redis raw query [%.*s] failed - no such key\n",attr->len,attr->s);
			freeReplyObject(reply);
			return -2;
		case REDIS_REPLY_STATUS:
			LM_DBG("Received a status of %.*s from Redis \n",(unsigned)reply->len,reply->str);
			if (reply_no)
				*reply_no = 0;
			freeReplyObject(reply);
			return 1;
		default:
			/* some data arrived - yay */

			if (rpl == NULL) {
				LM_DBG("Received reply type %d but script writer not interested in it \n",reply->type);
				freeReplyObject(reply);
				return 1;
			}
			return redis_raw_query_handle_reply(reply,rpl,expected_kv_no,reply_no);
	}

	return 1;
}
