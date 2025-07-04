/*++
/* NAME
/*	dict_mysql 3
/* SUMMARY
/*	dictionary manager interface to MySQL databases
/* SYNOPSIS
/*	#include <dict_mysql.h>
/*
/*	DICT	*dict_mysql_open(name, open_flags, dict_flags)
/*	const char *name;
/*	int	open_flags;
/*	int	dict_flags;
/* DESCRIPTION
/*	dict_mysql_open() creates a dictionary of type 'mysql'.  This
/*	dictionary is an interface for the postfix key->value mappings
/*	to mysql.  The result is a pointer to the installed dictionary,
/*	or a null pointer in case of problems.
/*
/*	The mysql dictionary can manage multiple connections to different
/*	sql servers on different hosts.  It assumes that the underlying data
/*	on each host is identical (mirrored) and maintains one connection
/*	at any given time.  If any connection fails,  any other available
/*	ones will be opened and used.  The intent of this feature is to eliminate
/*	a single point of failure for mail systems that would otherwise rely
/*	on a single mysql server.
/* .PP
/*	Arguments:
/* .IP name
/*	Either the path to the MySQL configuration file (if it starts
/*	with '/' or '.'), or the prefix which will be used to obtain
/*	main.cf configuration parameters for this search.
/*
/*	In the first case, the configuration parameters below are
/*	specified in the file as \fIname\fR=\fIvalue\fR pairs.
/*
/*	In the second case, the configuration parameters are
/*	prefixed with the value of \fIname\fR and an underscore,
/*	and they are specified in main.cf.  For example, if this
/*	value is \fImysqlsource\fR, the parameters would look like
/*	\fImysqlsource_user\fR, \fImysqlsource_table\fR, and so on.
/*
/* .IP other_name
/*	reference for outside use.
/* .IP open_flags
/*	Must be O_RDONLY.
/* .IP dict_flags
/*	See dict_open(3).
/* SEE ALSO
/*	dict(3) generic dictionary manager
/*	mysql_table(5) MySQL client configuration
/* AUTHOR(S)
/*	Scott Cotton, Joshua Marcus
/*	IC Group, Inc.
/*	scott@icgroup.com
/*
/*	Liviu Daia
/*	Institute of Mathematics of the Romanian Academy
/*	P.O. BOX 1-764
/*	RO-014700 Bucharest, ROMANIA
/*
/*	John Fawcett
/*
/*	Wietse Venema
/*	Google, Inc.
/*	111 8th Avenue
/*	New York, NY 10011, USA
/*--*/

/* System library. */
#include "sys_defs.h"

#ifdef HAS_MYSQL
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>
#include <time.h>
#include <mysql.h>
#include <limits.h>
#include <errno.h>

#if !defined(MYSQL_VERSION_ID) || MYSQL_VERSION_ID < 40000
#error "MySQL versions <4 are no longer supported"
#endif

#ifdef STRCASECMP_IN_STRINGS_H
#include <strings.h>
#endif

/* Utility library. */

#include "dict.h"
#include "msg.h"
#include "mymalloc.h"
#include "argv.h"
#include "vstring.h"
#include "split_at.h"
#include "find_inet.h"
#include "myrand.h"
#include "events.h"
#include "stringops.h"

/* Global library. */

#include "cfg_parser.h"
#include "db_common.h"

/* Application-specific. */

#include "dict_mysql.h"

/* MySQL 8.x API change */

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50023
#define DICT_MYSQL_SSL_VERIFY_SERVER_CERT MYSQL_OPT_SSL_VERIFY_SERVER_CERT
#elif MYSQL_VERSION_ID >= 80000
#define DICT_MYSQL_SSL_VERIFY_SERVER_CERT MYSQL_OPT_SSL_MODE
#endif

 /*
  * MariaDB Connector/C 3.0.0 lists mysql_options() as deprecated and
  * recommends using mysql_optionsv() instead. Option names and semantics
  * have not changed.
  */
#if defined(MARIADB_PACKAGE_VERSION_ID) && MARIADB_PACKAGE_VERSION_ID >= 30000
#define mysql_options	mysql_optionsv
#endif

/* need some structs to help organize things */
typedef struct {
    MYSQL  *db;
    char   *hostname;
    char   *name;
    unsigned port;
    unsigned type;			/* TYPEUNIX | TYPEINET */
    unsigned stat;			/* STATUNTRIED | STATFAIL | STATCUR */
    time_t  ts;				/* used for attempting reconnection
					 * every so often if a host is down */
} HOST;

typedef struct {
    int     len_hosts;			/* number of hosts */
    HOST  **db_hosts;			/* the hosts on which the databases
					 * reside */
} PLMYSQL;

typedef struct {
    DICT    dict;
    CFG_PARSER *parser;
    char   *query;
    char   *result_format;
    char   *option_file;
    char   *option_group;
    void   *ctx;
    int     expansion_limit;
    char   *username;
    char   *password;
    char   *dbname;
    char   *charset;
    int     retry_interval;
    int     idle_interval;
    ARGV   *hosts;
    PLMYSQL *pldb;
    HOST   *active_host;
    char   *tls_cert_file;
    char   *tls_key_file;
    char   *tls_CAfile;
    char   *tls_CApath;
    char   *tls_ciphers;
#if defined(DICT_MYSQL_SSL_VERIFY_SERVER_CERT)
    int     tls_verify_cert;
#endif
    int     require_result_set;
} DICT_MYSQL;

#define STATACTIVE			(1<<0)
#define STATFAIL			(1<<1)
#define STATUNTRIED			(1<<2)

#define TYPEUNIX			(1<<0)
#define TYPEINET			(1<<1)

#define RETRY_CONN_MAX			100
#define DEF_RETRY_INTV			60	/* 1 minute */
#define DEF_IDLE_INTV			60	/* 1 minute */

/* internal function declarations */
static PLMYSQL *plmysql_init(ARGV *);
static int plmysql_query(DICT_MYSQL *, const char *, VSTRING *, MYSQL_RES **);
static void plmysql_dealloc(PLMYSQL *);
static void plmysql_close_host(HOST *);
static void plmysql_down_host(HOST *, int);
static void plmysql_connect_single(DICT_MYSQL *, HOST *);
static const char *dict_mysql_lookup(DICT *, const char *);
DICT   *dict_mysql_open(const char *, int, int);
static void dict_mysql_close(DICT *);
static void mysql_parse_config(DICT_MYSQL *, const char *);
static HOST *host_init(const char *);

/* dict_mysql_quote - escape SQL metacharacters in input string */

static void dict_mysql_quote(DICT *dict, const char *name, VSTRING *result)
{
    DICT_MYSQL *dict_mysql = (DICT_MYSQL *) dict;
    int     len = strlen(name);
    int     buflen;

    /*
     * We won't get integer overflows in 2*len + 1, because Postfix input
     * keys have reasonable size limits, better safe than sorry.
     */
    if (len > (INT_MAX - VSTRING_LEN(result) - 1) / 2)
	msg_panic("dict_mysql_quote: integer overflow in %lu+2*%d+1",
		  (unsigned long) VSTRING_LEN(result), len);
    buflen = 2 * len + 1;
    VSTRING_SPACE(result, buflen);

    if (dict_mysql->active_host == 0)
	msg_panic("dict_mysql_quote: no active host");
#if MYSQL_VERSION_ID >= 50706 && !defined(MARIADB_VERSION_ID)
    mysql_real_escape_string_quote(dict_mysql->active_host->db,
				   vstring_end(result), name, len, '\'');
#else
    if (mysql_real_escape_string(dict_mysql->active_host->db,
				 vstring_end(result), name, len) ==
	(unsigned long) -1) {
	msg_warn("dict_mysql: host (%s) cannot escape input string: >%s<",
		 dict_mysql->active_host->hostname,
		 mysql_error(dict_mysql->active_host->db));
	dict_mysql->active_host->stat = STATFAIL;
    }
#endif

    VSTRING_SKIP(result);
}

/* dict_mysql_lookup - find database entry */

static const char *dict_mysql_lookup(DICT *dict, const char *name)
{
    const char *myname = "dict_mysql_lookup";
    DICT_MYSQL *dict_mysql = (DICT_MYSQL *) dict;
    MYSQL_RES *query_res;
    MYSQL_ROW row;
    static VSTRING *result;
    static VSTRING *query;
    int     i;
    int     j;
    int     numrows;
    int     expansion;
    const char *r;
    int     domain_rc;

    dict->error = 0;

    /*
     * Don't frustrate future attempts to make Postfix UTF-8 transparent.
     */
#ifdef SNAPSHOT
    if ((dict->flags & DICT_FLAG_UTF8_ACTIVE) == 0
	&& !valid_utf8_stringz(name)) {
	if (msg_verbose)
	    msg_info("%s: %s: Skipping lookup of non-UTF-8 key '%s'",
		     myname, dict_mysql->parser->name, name);
	return (0);
    }
#endif

    /*
     * Optionally fold the key.
     */
    if (dict->flags & DICT_FLAG_FOLD_FIX) {
	if (dict->fold_buf == 0)
	    dict->fold_buf = vstring_alloc(10);
	vstring_strcpy(dict->fold_buf, name);
	name = lowercase(vstring_str(dict->fold_buf));
    }

    /*
     * If there is a domain list for this map, then only search for addresses
     * in domains on the list. This can significantly reduce the load on the
     * server.
     */
    if ((domain_rc = db_common_check_domain(dict_mysql->ctx, name)) == 0) {
	if (msg_verbose)
	    msg_info("%s: Skipping lookup of '%s'", myname, name);
	return (0);
    }
    if (domain_rc < 0) {
	msg_warn("%s:%s 'domain' pattern match failed for '%s'",
		 dict->type, dict->name, name);
	DICT_ERR_VAL_RETURN(dict, domain_rc, (char *) 0);
    }
#define INIT_VSTR(buf, len) do { \
	if (buf == 0) \
	    buf = vstring_alloc(len); \
	VSTRING_RESET(buf); \
	VSTRING_TERMINATE(buf); \
    } while (0)

    INIT_VSTR(query, 10);

    /*
     * Suppress the lookup if the query expansion is empty
     * 
     * This initial expansion is outside the context of any specific host
     * connection, we just want to check the key pre-requisites, so when
     * quoting happens separately for each connection, we don't bother with
     * quoting...
     */
    if (!db_common_expand(dict_mysql->ctx, dict_mysql->query,
			  name, 0, query, (db_quote_callback_t) 0))
	return (0);

    /* do the query - set dict->error & cleanup if there's an error */
    if (plmysql_query(dict_mysql, name, query, &query_res) == 0) {
	dict->error = DICT_ERR_RETRY;
	return (0);
    }
    if (query_res == 0)
	return (0);
    numrows = mysql_num_rows(query_res);
    if (msg_verbose)
	msg_info("%s: retrieved %d rows", myname, numrows);
    if (numrows == 0) {
	mysql_free_result(query_res);
	return 0;
    }
    INIT_VSTR(result, 10);

    for (expansion = i = 0; i < numrows && dict->error == 0; i++) {
	row = mysql_fetch_row(query_res);
	for (j = 0; j < mysql_num_fields(query_res); j++) {
	    if (db_common_expand(dict_mysql->ctx, dict_mysql->result_format,
				 row[j], name, result, 0)
		&& dict_mysql->expansion_limit > 0
		&& ++expansion > dict_mysql->expansion_limit) {
		msg_warn("%s: %s: Expansion limit exceeded for key: '%s'",
			 myname, dict_mysql->parser->name, name);
		dict->error = DICT_ERR_RETRY;
		break;
	    }
	}
    }
    mysql_free_result(query_res);
    r = vstring_str(result);
    return ((dict->error == 0 && *r) ? r : 0);
}

/* dict_mysql_check_stat - check the status of a host */

static int dict_mysql_check_stat(HOST *host, unsigned stat, unsigned type,
				         time_t t)
{
    if ((host->stat & stat) && (!type || host->type & type)) {
	/* try not to hammer the dead hosts too often */
	if (host->stat == STATFAIL && host->ts > 0 && host->ts >= t)
	    return 0;
	return 1;
    }
    return 0;
}

/* dict_mysql_find_host - find a host with the given status */

static HOST *dict_mysql_find_host(PLMYSQL *PLDB, unsigned stat, unsigned type)
{
    time_t  t;
    int     count = 0;
    int     idx;
    int     i;

    t = time((time_t *) 0);
    for (i = 0; i < PLDB->len_hosts; i++) {
	if (dict_mysql_check_stat(PLDB->db_hosts[i], stat, type, t))
	    count++;
    }

    if (count) {
	idx = (count > 1) ?
	    1 + count * (double) myrand() / (1.0 + RAND_MAX) : 1;

	for (i = 0; i < PLDB->len_hosts; i++) {
	    if (dict_mysql_check_stat(PLDB->db_hosts[i], stat, type, t) &&
		--idx == 0)
		return PLDB->db_hosts[i];
	}
    }
    return 0;
}

/* dict_mysql_get_active - get an active connection */

static HOST *dict_mysql_get_active(DICT_MYSQL *dict_mysql)
{
    const char *myname = "dict_mysql_get_active";
    PLMYSQL *PLDB = dict_mysql->pldb;
    HOST   *host;
    int     count = RETRY_CONN_MAX;

    /* Try the active connections first; prefer the ones to UNIX sockets. */
    if ((host = dict_mysql_find_host(PLDB, STATACTIVE, TYPEUNIX)) != NULL ||
	(host = dict_mysql_find_host(PLDB, STATACTIVE, TYPEINET)) != NULL) {
	if (msg_verbose)
	    msg_info("%s: found active connection to host %s", myname,
		     host->hostname);
	return host;
    }

    /*
     * Try the remaining hosts. "count" is a safety net, in case the loop
     * takes more than RETRY_CONN_INTV and the dead hosts are no longer
     * skipped.
     */
    while (--count > 0 &&
	   ((host = dict_mysql_find_host(PLDB, STATUNTRIED | STATFAIL,
					 TYPEUNIX)) != NULL ||
	    (host = dict_mysql_find_host(PLDB, STATUNTRIED | STATFAIL,
					 TYPEINET)) != NULL)) {
	if (msg_verbose)
	    msg_info("%s: attempting to connect to host %s", myname,
		     host->hostname);
	plmysql_connect_single(dict_mysql, host);
	if (host->stat == STATACTIVE)
	    return host;
    }

    /* bad news... */
    return 0;
}

/* dict_mysql_event - callback: close idle connections */

static void dict_mysql_event(int unused_event, void *context)
{
    HOST   *host = (HOST *) context;

    if (host->db)
	plmysql_close_host(host);
}

/*
 * plmysql_query - process a MySQL query.  Return 'true' on success.
 *			On failure, log failure and try other db instances.
 *			on failure of all db instances, return 'false';
 *			close unnecessary active connections
 */

static int plmysql_query(DICT_MYSQL *dict_mysql,
			         const char *name,
			         VSTRING *query,
			         MYSQL_RES **result)
{
    HOST   *host;
    MYSQL_RES *first_result = 0;

    /* In case all hosts are down. */
    int     query_error = 1;

    errno = ENOTSUP;

    /*
     * Helper to avoid spamming the log with warnings.
     */
#define SET_ERROR_AND_WARN_ONCE(err, ...) \
    do { \
	if (err == 0) { \
	    err = 1; \
	    msg_warn(__VA_ARGS__); \
	} \
    } while (0)

    while ((host = dict_mysql_get_active(dict_mysql)) != NULL) {

	/*
	 * The active host is used to escape strings in the context of the
	 * active connection's character encoding.
	 */
	dict_mysql->active_host = host;
	VSTRING_RESET(query);
	VSTRING_TERMINATE(query);
	db_common_expand(dict_mysql->ctx, dict_mysql->query,
			 name, 0, query, dict_mysql_quote);
	/* Check for potential dict_mysql_quote() failure. */
	if (host->stat == STATFAIL) {
	    plmysql_down_host(host, dict_mysql->retry_interval);
	    continue;
	}
	if (msg_verbose)
	    msg_info("expanded and quoted query: >%s<", vstring_str(query));
	dict_mysql->active_host = 0;

	query_error = 0;
	errno = 0;

	/*
	 * The query must complete.
	 */
	if (mysql_query(host->db, vstring_str(query)) != 0) {
	    query_error = 1;
	    msg_warn("%s:%s: query failed: %s",
		     dict_mysql->dict.type, dict_mysql->dict.name,
		     mysql_error(host->db));
	}

	/*
	 * Collect all result sets to avoid synchronization errors.
	 */
	else {
	    int     next_res_status;

	    do {
		MYSQL_RES *temp_result;

		/*
		 * Keep the first result set. Reject multiple result sets.
		 */
		if ((temp_result = mysql_store_result(host->db)) != 0) {
		    if (first_result == 0) {
			first_result = temp_result;
		    } else {
			SET_ERROR_AND_WARN_ONCE(query_error,
				"%s:%s: query failed: multiple result sets "
					 "returning data are not supported",
						dict_mysql->dict.type,
						dict_mysql->dict.name);
			mysql_free_result(temp_result);
		    }
		}

		/*
		 * No result: the mysql_field_count() function must return 0
		 * to indicate that mysql_store_result() completed normally.
		 */
		else if (mysql_field_count(host->db) != 0) {
		    SET_ERROR_AND_WARN_ONCE(query_error,
			     "%s:%s: query failed (mysql_store_result): %s",
					    dict_mysql->dict.type,
					    dict_mysql->dict.name,
					    mysql_error(host->db));
		}

		/*
		 * Are there more results? -1 = no, 0 = yes, > 0 = error.
		 */
		if ((next_res_status = mysql_next_result(host->db)) > 0) {
		    SET_ERROR_AND_WARN_ONCE(query_error,
			      "%s:%s: query failed (mysql_next_result): %s",
					    dict_mysql->dict.type,
					    dict_mysql->dict.name,
					    mysql_error(host->db));
		}
	    } while (next_res_status == 0);

	    /*
	     * Enforce the require_result_set setting.
	     */
	    if (first_result == 0 && dict_mysql->require_result_set) {
		SET_ERROR_AND_WARN_ONCE(query_error,
			 "%s:%s: query failed: query returned no result set"
					"(require_result_set = yes)",
					dict_mysql->dict.type,
					dict_mysql->dict.name);
	    }
	}

	/*
	 * See what we got.
	 */
	if (query_error) {
	    plmysql_down_host(host, dict_mysql->retry_interval);
	    if (errno == 0)
		errno = ENOTSUP;
	    if (first_result) {
		mysql_free_result(first_result);
		first_result = 0;
	    }
	} else {
	    if (msg_verbose)
		msg_info("%s:%s: successful query result from host %s",
			 dict_mysql->dict.type, dict_mysql->dict.name,
			 host->hostname);
	    event_request_timer(dict_mysql_event, (void *) host,
				dict_mysql->idle_interval);
	    break;
	}
    }

    *result = first_result;
    return (query_error == 0);
}

/*
 * plmysql_connect_single -
 * used to reconnect to a single database when one is down or none is
 * connected yet. Log all errors and set the stat field of host accordingly
 */
static void plmysql_connect_single(DICT_MYSQL *dict_mysql, HOST *host)
{
    if ((host->db = mysql_init(NULL)) == NULL)
	msg_fatal("dict_mysql: insufficient memory");
    if (dict_mysql->option_file)
	mysql_options(host->db, MYSQL_READ_DEFAULT_FILE, dict_mysql->option_file);
    if (dict_mysql->option_group && dict_mysql->option_group[0])
	mysql_options(host->db, MYSQL_READ_DEFAULT_GROUP, dict_mysql->option_group);
#if MYSQL_VERSION_ID >= 80035
    /* Preferred API. */
    if (dict_mysql->tls_key_file)
	mysql_options(host->db, MYSQL_OPT_SSL_KEY, dict_mysql->tls_key_file);
    if (dict_mysql->tls_cert_file)
	mysql_options(host->db, MYSQL_OPT_SSL_CERT, dict_mysql->tls_cert_file);
    if (dict_mysql->tls_CAfile)
	mysql_options(host->db, MYSQL_OPT_SSL_CA, dict_mysql->tls_CAfile);
    if (dict_mysql->tls_CApath)
	mysql_options(host->db, MYSQL_OPT_SSL_CAPATH, dict_mysql->tls_CApath);
    if (dict_mysql->tls_ciphers)
	mysql_options(host->db, MYSQL_OPT_SSL_CIPHER, dict_mysql->tls_ciphers);
#else
    /* Deprecated API. */
    if (dict_mysql->tls_key_file || dict_mysql->tls_cert_file ||
	dict_mysql->tls_CAfile || dict_mysql->tls_CApath || dict_mysql->tls_ciphers)
	mysql_ssl_set(host->db,
		      dict_mysql->tls_key_file, dict_mysql->tls_cert_file,
		      dict_mysql->tls_CAfile, dict_mysql->tls_CApath,
		      dict_mysql->tls_ciphers);
#endif
#if defined(DICT_MYSQL_SSL_VERIFY_SERVER_CERT)
    if (dict_mysql->tls_verify_cert != -1)
	mysql_options(host->db, DICT_MYSQL_SSL_VERIFY_SERVER_CERT,
		      &dict_mysql->tls_verify_cert);
#endif
    if (mysql_real_connect(host->db,
			   (host->type == TYPEINET ? host->name : 0),
			   dict_mysql->username,
			   dict_mysql->password,
			   dict_mysql->dbname,
			   host->port,
			   (host->type == TYPEUNIX ? host->name : 0),
			   CLIENT_MULTI_RESULTS)) {
	if (mysql_set_character_set(host->db, dict_mysql->charset) != 0) {
	    msg_warn("dict_mysql: mysql_set_character_set '%s' failed: %s",
		     dict_mysql->charset, mysql_error(host->db));
	    plmysql_down_host(host, dict_mysql->retry_interval);
	    return;
	}
	if (msg_verbose)
	    msg_info("dict_mysql: successful connection to host %s",
		     host->hostname);
	host->stat = STATACTIVE;
    } else {
	msg_warn("connect to mysql server %s: %s",
		 host->hostname, mysql_error(host->db));
	plmysql_down_host(host, dict_mysql->retry_interval);
    }
}

/* plmysql_close_host - close an established MySQL connection */
static void plmysql_close_host(HOST *host)
{
    mysql_close(host->db);
    host->db = 0;
    host->stat = STATUNTRIED;
}

/*
 * plmysql_down_host - close a failed connection AND set a "stay away from
 * this host" timer
 */
static void plmysql_down_host(HOST *host, int retry_interval)
{
    mysql_close(host->db);
    host->db = 0;
    host->ts = time((time_t *) 0) + retry_interval;
    host->stat = STATFAIL;
    event_cancel_timer(dict_mysql_event, (void *) host);
}

/* mysql_parse_config - parse mysql configuration file */

static void mysql_parse_config(DICT_MYSQL *dict_mysql, const char *mysqlcf)
{
    const char *myname = "mysql_parse_config";
    CFG_PARSER *p = dict_mysql->parser;
    VSTRING *buf;
    char   *hosts;

    dict_mysql->username = cfg_get_str(p, "user", "", 0, 0);
    dict_mysql->password = cfg_get_str(p, "password", "", 0, 0);
    dict_mysql->dbname = cfg_get_str(p, "dbname", "", 1, 0);
    dict_mysql->charset = cfg_get_str(p, "charset", "utf8mb4", 1, 0);
    dict_mysql->retry_interval = cfg_get_int(p, "retry_interval",
					     DEF_RETRY_INTV, 1, 0);
    dict_mysql->idle_interval = cfg_get_int(p, "idle_interval",
					    DEF_IDLE_INTV, 1, 0);
    dict_mysql->result_format = cfg_get_str(p, "result_format", "%s", 1, 0);
    dict_mysql->option_file = cfg_get_str(p, "option_file", NULL, 0, 0);
    dict_mysql->option_group = cfg_get_str(p, "option_group", "client", 0, 0);
    dict_mysql->tls_key_file = cfg_get_str(p, "tls_key_file", NULL, 0, 0);
    dict_mysql->tls_cert_file = cfg_get_str(p, "tls_cert_file", NULL, 0, 0);
    dict_mysql->tls_CAfile = cfg_get_str(p, "tls_CAfile", NULL, 0, 0);
    dict_mysql->tls_CApath = cfg_get_str(p, "tls_CApath", NULL, 0, 0);
    dict_mysql->tls_ciphers = cfg_get_str(p, "tls_ciphers", NULL, 0, 0);
#if defined(DICT_MYSQL_SSL_VERIFY_SERVER_CERT)
    dict_mysql->tls_verify_cert = cfg_get_bool(p, "tls_verify_cert", -1);
#endif
    dict_mysql->require_result_set = cfg_get_bool(p, "require_result_set", 1);

    /*
     * XXX: The default should be non-zero for safety, but that is not
     * backwards compatible.
     */
    dict_mysql->expansion_limit = cfg_get_int(dict_mysql->parser,
					      "expansion_limit", 0, 0, 0);

    if ((dict_mysql->query = cfg_get_str(p, "query", NULL, 0, 0)) == 0) {

	/*
	 * No query specified -- fallback to building it from components (old
	 * style "select %s from %s where %s")
	 */
	buf = vstring_alloc(64);
	db_common_sql_build_query(buf, p);
	dict_mysql->query = vstring_export(buf);
    }

    /*
     * Must parse all templates before we can use db_common_expand()
     */
    dict_mysql->ctx = 0;
    (void) db_common_parse(&dict_mysql->dict, &dict_mysql->ctx,
			   dict_mysql->query, 1);
    (void) db_common_parse(0, &dict_mysql->ctx, dict_mysql->result_format, 0);
    db_common_parse_domain(p, dict_mysql->ctx);

    /*
     * Maps that use substring keys should only be used with the full input
     * key.
     */
    if (db_common_dict_partial(dict_mysql->ctx))
	dict_mysql->dict.flags |= DICT_FLAG_PATTERN;
    else
	dict_mysql->dict.flags |= DICT_FLAG_FIXED;
    if (dict_mysql->dict.flags & DICT_FLAG_FOLD_FIX)
	dict_mysql->dict.fold_buf = vstring_alloc(10);

    hosts = cfg_get_str(p, "hosts", "", 0, 0);

    dict_mysql->hosts = argv_split(hosts, CHARS_COMMA_SP);
    if (dict_mysql->hosts->argc == 0) {
	argv_add(dict_mysql->hosts, "localhost", ARGV_END);
	argv_terminate(dict_mysql->hosts);
	if (msg_verbose)
	    msg_info("%s: %s: no hostnames specified, defaulting to '%s'",
		     myname, mysqlcf, dict_mysql->hosts->argv[0]);
    }
    /* Don't blacklist the load balancer! */
    if (dict_mysql->hosts->argc == 1)
	argv_add(dict_mysql->hosts, dict_mysql->hosts->argv[0], (char *) 0);
    myfree(hosts);
}

/* dict_mysql_open - open MYSQL data base */

DICT   *dict_mysql_open(const char *name, int open_flags, int dict_flags)
{
    DICT_MYSQL *dict_mysql;
    CFG_PARSER *parser;

    /*
     * Sanity checks.
     */
    if (open_flags != O_RDONLY)
	return (dict_surrogate(DICT_TYPE_MYSQL, name, open_flags, dict_flags,
			       "%s:%s map requires O_RDONLY access mode",
			       DICT_TYPE_MYSQL, name));

    /*
     * Open the configuration file.
     */
    if ((parser = cfg_parser_alloc(name)) == 0)
	return (dict_surrogate(DICT_TYPE_MYSQL, name, open_flags, dict_flags,
			       "open %s: %m", name));

    dict_mysql = (DICT_MYSQL *) dict_alloc(DICT_TYPE_MYSQL, name,
					   sizeof(DICT_MYSQL));
    dict_mysql->dict.lookup = dict_mysql_lookup;
    dict_mysql->dict.close = dict_mysql_close;
    dict_mysql->dict.flags = dict_flags;
    dict_mysql->parser = parser;
    mysql_parse_config(dict_mysql, name);
    dict_mysql->active_host = 0;
    dict_mysql->pldb = plmysql_init(dict_mysql->hosts);
    if (dict_mysql->pldb == NULL)
	msg_fatal("couldn't initialize pldb!\n");
    dict_mysql->dict.owner = cfg_get_owner(dict_mysql->parser);
    return (&dict_mysql->dict);
}

/*
 * plmysql_init - initialize a MYSQL database.
 *		    Return NULL on failure, or a PLMYSQL * on success.
 */
static PLMYSQL *plmysql_init(ARGV *hosts)
{
    PLMYSQL *PLDB;
    int     i;

    if ((PLDB = (PLMYSQL *) mymalloc(sizeof(PLMYSQL))) == 0)
	msg_fatal("mymalloc of pldb failed");

    PLDB->len_hosts = hosts->argc;
    if ((PLDB->db_hosts = (HOST **) mymalloc(sizeof(HOST *) * hosts->argc)) == 0)
	return (0);
    for (i = 0; i < hosts->argc; i++)
	PLDB->db_hosts[i] = host_init(hosts->argv[i]);

    return PLDB;
}


/* host_init - initialize HOST structure */
static HOST *host_init(const char *hostname)
{
    const char *myname = "mysql host_init";
    HOST   *host = (HOST *) mymalloc(sizeof(HOST));
    const char *d = hostname;
    char   *s;

    host->db = 0;
    host->hostname = mystrdup(hostname);
    host->port = 0;
    host->stat = STATUNTRIED;
    host->ts = 0;

    /*
     * Ad-hoc parsing code. Expect "unix:pathname" or "inet:host:port", where
     * both "inet:" and ":port" are optional.
     */
    if (strncmp(d, "unix:", 5) == 0) {
	d += 5;
	host->type = TYPEUNIX;
    } else {
	if (strncmp(d, "inet:", 5) == 0)
	    d += 5;
	host->type = TYPEINET;
    }
    host->name = mystrdup(d);
    if ((s = split_at_right(host->name, ':')) != 0)
	host->port = ntohs(find_inet_port(s, "tcp"));
    if (strcasecmp(host->name, "localhost") == 0) {
	/* The MySQL way: this will actually connect over the UNIX socket */
	myfree(host->name);
	host->name = 0;
	host->type = TYPEUNIX;
    }
    if (msg_verbose > 1)
	msg_info("%s: host=%s, port=%d, type=%s", myname,
		 host->name ? host->name : "localhost",
		 host->port, host->type == TYPEUNIX ? "unix" : "inet");
    return host;
}

/* dict_mysql_close - close MYSQL database */

static void dict_mysql_close(DICT *dict)
{
    DICT_MYSQL *dict_mysql = (DICT_MYSQL *) dict;

    plmysql_dealloc(dict_mysql->pldb);
    cfg_parser_free(dict_mysql->parser);
    myfree(dict_mysql->username);
    myfree(dict_mysql->password);
    myfree(dict_mysql->dbname);
    myfree(dict_mysql->charset);
    myfree(dict_mysql->query);
    myfree(dict_mysql->result_format);
    if (dict_mysql->option_file)
	myfree(dict_mysql->option_file);
    if (dict_mysql->option_group)
	myfree(dict_mysql->option_group);
    if (dict_mysql->tls_key_file)
	myfree(dict_mysql->tls_key_file);
    if (dict_mysql->tls_cert_file)
	myfree(dict_mysql->tls_cert_file);
    if (dict_mysql->tls_CAfile)
	myfree(dict_mysql->tls_CAfile);
    if (dict_mysql->tls_CApath)
	myfree(dict_mysql->tls_CApath);
    if (dict_mysql->tls_ciphers)
	myfree(dict_mysql->tls_ciphers);
    if (dict_mysql->hosts)
	argv_free(dict_mysql->hosts);
    if (dict_mysql->ctx)
	db_common_free_ctx(dict_mysql->ctx);
    if (dict->fold_buf)
	vstring_free(dict->fold_buf);
    dict_free(dict);
}

/* plmysql_dealloc - free memory associated with PLMYSQL close databases */
static void plmysql_dealloc(PLMYSQL *PLDB)
{
    int     i;

    for (i = 0; i < PLDB->len_hosts; i++) {
	event_cancel_timer(dict_mysql_event, (void *) (PLDB->db_hosts[i]));
	if (PLDB->db_hosts[i]->db)
	    mysql_close(PLDB->db_hosts[i]->db);
	myfree(PLDB->db_hosts[i]->hostname);
	if (PLDB->db_hosts[i]->name)
	    myfree(PLDB->db_hosts[i]->name);
	myfree((void *) PLDB->db_hosts[i]);
    }
    myfree((void *) PLDB->db_hosts);
    myfree((void *) (PLDB));
}

#endif
