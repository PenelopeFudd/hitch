/**
 * configuration.c
 *
 * Original author: Brane F. Gracnar
 */

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include <unistd.h>
#include <getopt.h>
#include <pwd.h>
#include <grp.h>
#include <syslog.h>
#include <libgen.h>
#include <limits.h>

#include "configuration.h"
#include "foreign/miniobj.h"
#include "foreign/vas.h"
#include "foreign/vsb.h"

#include "cfg_parser.h"

#define ADDR_LEN 150
#define PORT_LEN 6
#define CFG_BOOL_ON "on"


// BEGIN: configuration parameters
#define CFG_CIPHERS "ciphers"
#define CFG_SSL_ENGINE "ssl-engine"
#define CFG_PREFER_SERVER_CIPHERS "prefer-server-ciphers"
#define CFG_BACKEND "backend"
#define CFG_FRONTEND "frontend"
#define CFG_WORKERS "workers"
#define CFG_BACKLOG "backlog"
#define CFG_KEEPALIVE "keepalive"
#define CFG_BACKEND_REFRESH "backendrefresh"
#define CFG_CHROOT "chroot"
#define CFG_USER "user"
#define CFG_GROUP "group"
#define CFG_QUIET "quiet"
#define CFG_SYSLOG "syslog"
#define CFG_SYSLOG_FACILITY "syslog-facility"
#define CFG_PARAM_SYSLOG_FACILITY 11015
#define CFG_PARAM_SEND_BUFSIZE 11016
#define CFG_PARAM_RECV_BUFSIZE 11017
#define CFG_DAEMON "daemon"
#define CFG_WRITE_IP "write-ip"
#define CFG_WRITE_PROXY "write-proxy"
#define CFG_WRITE_PROXY_V1 "write-proxy-v1"
#define CFG_WRITE_PROXY_V2 "write-proxy-v2"
#define CFG_PEM_FILE "pem-file"
#define CFG_PROXY_PROXY "proxy-proxy"
#define CFG_ALPN_PROTOS "alpn-protos"
#define CFG_PARAM_ALPN_PROTOS 48173
#define CFG_BACKEND_CONNECT_TIMEOUT "backend-connect-timeout"
#define CFG_SSL_HANDSHAKE_TIMEOUT "ssl-handshake-timeout"
#define CFG_RECV_BUFSIZE "recv-bufsize"
#define CFG_SEND_BUFSIZE "send-bufsize"
#define CFG_LOG_FILENAME "log-filename"
#define CFG_LOG_LEVEL "log-level"
#define CFG_RING_SLOTS "ring-slots"
#define CFG_RING_DATA_LEN "ring-data-len"
#define CFG_PIDFILE "pidfile"
#define CFG_SNI_NOMATCH_ABORT "sni-nomatch-abort"
#define CFG_OCSP_DIR "ocsp-dir"
#define CFG_TLS_PROTOS "tls-protos"
#define CFG_PARAM_TLS_PROTOS 11018
#define CFG_DBG_LISTEN "dbg-listen"
#define CFG_PARAM_DBG_LISTEN 11019
#ifdef TCP_FASTOPEN_WORKS
	#define CFG_TFO "enable-tcp-fastopen"
#endif

#ifdef USE_SHARED_CACHE
	#define CFG_SHARED_CACHE "shared-cache"
	#define CFG_SHARED_CACHE_LISTEN "shared-cache-listen"
	#define CFG_SHARED_CACHE_PEER "shared-cache-peer"
	#define CFG_SHARED_CACHE_MCASTIF "shared-cache-if"
#endif

#define FMT_STR "%s = %s\n"
#define FMT_QSTR "%s = \"%s\"\n"
#define FMT_ISTR "%s = %d\n"

#define CONFIG_BUF_SIZE 1024
#define CFG_PARAM_CFGFILE 10000

#define CFG_CONFIG "config"

#define CFG_DEFAULT_CIPHERS \
	"EECDH+AESGCM:EDH+AESGCM:AES256+EECDH:AES256+EDH"

extern FILE *yyin;
extern int yyparse(hitch_config *);

void cfg_cert_file_free(struct cfg_cert_file **cfptr);

// END: configuration parameters

static char error_buf[CONFIG_BUF_SIZE];
static char tmp_buf[150];

/* declare printf like functions: */
void config_error_set(char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

void
config_error_set(char *fmt, ...)
{
	int len;
	char buf[CONFIG_BUF_SIZE] = "";

	va_list args;
	va_start(args, fmt);
	len = vsnprintf(buf, (sizeof(buf) - 1), fmt, args);
	va_end(args);

	len += 1;
	if (len > CONFIG_BUF_SIZE)
		len = CONFIG_BUF_SIZE;
	memcpy(error_buf, buf, len);
}

const char *
config_error_get(void)
{
	return (error_buf);
}

struct front_arg *
front_arg_new(void)
{
	struct front_arg *fa;

	ALLOC_OBJ(fa, FRONT_ARG_MAGIC);
	AN(fa);
	fa->match_global_certs = -1;
	fa->sni_nomatch_abort = -1;
	fa->selected_protos = 0;
	fa->prefer_server_ciphers = -1;
	fa->client_verify = -1;

	return (fa);
}

void
front_arg_destroy(struct front_arg *fa)
{
	struct cfg_cert_file *cf, *cftmp;

	CHECK_OBJ_NOTNULL(fa, FRONT_ARG_MAGIC);
	free(fa->ip);
	free(fa->port);
	free(fa->pspec);
	free(fa->ciphers_tlsv12);
	free(fa->ciphersuites_tlsv13);
	HASH_ITER(hh, fa->certs, cf, cftmp) {
		CHECK_OBJ_NOTNULL(cf, CFG_CERT_FILE_MAGIC);
		HASH_DEL(fa->certs, cf);
		cfg_cert_file_free(&cf);
	}
	FREE_OBJ(fa);
}

hitch_config *
config_new(void)
{
	int i;
	hitch_config *r;
	struct front_arg *fa;

	r = calloc(1, sizeof(hitch_config));
	AN(r);

	(void) i;
	// set default values

	r->PMODE			= SSL_SERVER;
	r->SELECTED_TLS_PROTOS		= 0;
	r->WRITE_IP_OCTET		= 0;
	r->WRITE_PROXY_LINE_V1		= 0;
	r->WRITE_PROXY_LINE_V2		= 0;
	r->PROXY_TLV			= 1;
	r->PROXY_AUTHORITY		= 1;
	r->PROXY_CLIENT_CERT		= 0;
	r->PROXY_PROXY_LINE		= 0;
	r->ALPN_PROTOS			= NULL;
	r->ALPN_PROTOS_LV		= NULL;
	r->ALPN_PROTOS_LV_LEN		= 0;
	r->CHROOT			= NULL;
	r->UID				= -1;
	r->GID				= -1;
	r->BACK_IP			= strdup("127.0.0.1");
	r->BACK_PORT			= strdup("8000");
	r->NCORES			= 1;
	r->CIPHERS_TLSv12		= strdup(CFG_DEFAULT_CIPHERS);
	r->ENGINE			= NULL;
	r->BACKLOG			= 100;
	r->SNI_NOMATCH_ABORT		= 0;
	r->CERT_DEFAULT			= NULL;
	r->CERT_FILES			= NULL;
	r->LISTEN_ARGS			= NULL;
	r->PEM_DIR			= NULL;
	r->OCSP_DIR			= strdup("/var/lib/hitch/");
	AN(r->OCSP_DIR);
	r->OCSP_VFY			= 0;
	r->OCSP_RESP_TMO		= 10.0;
	r->OCSP_CONN_TMO		= 4.0;
	r->OCSP_REFRESH_INTERVAL	= 1800;
	r->CLIENT_VERIFY		= SSL_VERIFY_NONE;
	r->CLIENT_VERIFY_CA		= NULL;
#ifdef TCP_FASTOPEN_WORKS
	r->TFO				= 0;
#endif

#ifdef USE_SHARED_CACHE
	r->SHARED_CACHE			= 0;
	r->SHCUPD_IP			= NULL;
	r->SHCUPD_PORT			= NULL;

	for (i = 0 ; i < MAX_SHCUPD_PEERS; i++)
		memset(&r->SHCUPD_PEERS[i], 0, sizeof(shcupd_peer_opt));

	r->SHCUPD_MCASTIF		= NULL;
	r->SHCUPD_MCASTTTL		= NULL;
#endif

	r->LOG_LEVEL			= 1;
	r->SYSLOG			= 0;
	r->SYSLOG_FACILITY		= LOG_DAEMON;
	r->TCP_KEEPALIVE_TIME		= 3600;
	r->BACKEND_REFRESH_TIME		= 0;
	r->DAEMONIZE			= 0;
	r->PREFER_SERVER_CIPHERS	= 0;
	r->TEST				= 0;

	r->BACKEND_CONNECT_TIMEOUT	= 30;
	r->SSL_HANDSHAKE_TIMEOUT	= 30;

	r->RECV_BUFSIZE			= -1;
	r->SEND_BUFSIZE			= -1;

	r->LOG_FILENAME			= NULL;
	r->PIDFILE			= NULL;

	r->RING_SLOTS			= 0;
	r->RING_DATA_LEN		= 0;

	fa = front_arg_new();
	fa->port = strdup("8443");
	AN(fa->port);
	fa->pspec = strdup("default");
	AN(fa->pspec);
	HASH_ADD_KEYPTR(hh, r->LISTEN_ARGS, fa->pspec, strlen(fa->pspec), fa);
	r->LISTEN_DEFAULT		= fa;

	return (r);
}

void
config_destroy(hitch_config *cfg)
{
	// printf("config_destroy() in pid %d: %p\n", getpid(), cfg);
	struct front_arg *fa, *ftmp;
	struct cfg_cert_file *cf, *cftmp;
	if (cfg == NULL)
		return;

	// free all members!
	free(cfg->CHROOT);
	HASH_ITER(hh, cfg->LISTEN_ARGS, fa, ftmp) {
		CHECK_OBJ_NOTNULL(fa, FRONT_ARG_MAGIC);
		HASH_DEL(cfg->LISTEN_ARGS, fa);
		front_arg_destroy(fa);
	}
	free(cfg->BACK_IP);
	free(cfg->BACK_PORT);
	HASH_ITER(hh, cfg->CERT_FILES, cf, cftmp) {
		CHECK_OBJ_NOTNULL(cf, CFG_CERT_FILE_MAGIC);
		HASH_DEL(cfg->CERT_FILES, cf);
		cfg_cert_file_free(&cf);
	}

	if (cfg->CERT_DEFAULT != NULL)
		cfg_cert_file_free(&cfg->CERT_DEFAULT);

	free(cfg->CIPHERS_TLSv12);
	free(cfg->CIPHERSUITES_TLSv13);
	free(cfg->ENGINE);
	free(cfg->PIDFILE);
	free(cfg->OCSP_DIR);
	free(cfg->ALPN_PROTOS);
	free(cfg->ALPN_PROTOS_LV);
	free(cfg->PEM_DIR);
	free(cfg->PEM_DIR_GLOB);
	free(cfg->CLIENT_VERIFY_CA);
#ifdef USE_SHARED_CACHE
	int i;
	free(cfg->SHCUPD_IP);
	free(cfg->SHCUPD_PORT);

	for (i = 0; i < MAX_SHCUPD_PEERS; i++) {
		free(cfg->SHCUPD_PEERS[i].ip);
		free(cfg->SHCUPD_PEERS[i].port);
	}

	free(cfg->SHCUPD_MCASTIF);
	free(cfg->SHCUPD_MCASTTTL);
#endif
	free(cfg);
}

static char *
config_assign_str(char **dst, char *v)
{
	assert(v != NULL);

	if (strlen(v) <= 0)
		return (NULL);
	if (*dst != NULL)
		free(*dst);

	*dst = strdup(v);
	return (*dst);
}

static int
config_param_val_bool(char *val, int *res)
{
	assert(val != NULL);

	if (strcasecmp(val, CFG_BOOL_ON) == 0 || strcasecmp(val, "yes") == 0 ||
	    strcasecmp(val, "y") == 0 || strcasecmp(val, "true") == 0 ||
	    strcasecmp(val, "t") == 0 || strcasecmp(val, "1") == 0)
		*res = 1;
	else if (strcasecmp(val, "off") == 0 || strcasecmp(val, "no") == 0
	    || strcasecmp(val, "n") == 0 || strcasecmp(val, "false") == 0
	    || strcasecmp(val, "f") == 0 || strcasecmp(val, "0") == 0)
		*res = 0;

	return (1);
}

static int
config_param_uds(const char *str, char **path)
{
	struct stat st;

	AN(path);

	if (strlen(str) > 104) {
		config_error_set("UNIX domain socket path too long.");
		return (0);
	}

	if (stat(str, &st)) {
		config_error_set("Unable to stat path '%s': %s", str,
		    strerror(errno));
		return (0);
	}

	if (!S_ISSOCK(st.st_mode)) {
		config_error_set("Invalid path '%s': Not a socket.", str);
		return (0);
	}

	*path = strdup(str);
	return (1);
}

static int
config_param_host_port_wildcard(const char *str, char **addr,
    char **port, char **cert, int wildcard_okay, char **path)
{
	const char *cert_ptr = NULL;
	char port_buf[PORT_LEN];
	char addr_buf[ADDR_LEN];

	if (str == NULL) {
		config_error_set("Invalid/unset host/port string.");
		return (0);
	}

	/* UDS addresses start with a '/' */
	if (path != NULL && *str == '/') {
		*addr = NULL;
		*port = NULL;
		return (config_param_uds(str, path));
	}

	if (strlen(str) > ADDR_LEN) {
		config_error_set("Host address too long.");
		return (0);
	}

	memset(port_buf, '\0', sizeof(port_buf));
	memset(addr_buf, '\0', sizeof(addr_buf));

	// FORMAT IS: [address]:port
	if (*str != '[') {
		config_error_set("Invalid address string '%s'", str);
		return (0);
	}

	const char *ptr = str + 1;
	const char *x = strrchr(ptr, ']');
	if (x == NULL) {
		config_error_set("Invalid address '%s'.", str);
		return (0);
	}

	unsigned addrlen = x - ptr;
	// address
	if (addrlen >= sizeof(addr_buf)) {
		config_error_set("Invalid address '%s'.", str);
		return (0);
	}
	strncpy(addr_buf, ptr, addrlen);

	// port
	if (x[1] != ':' || x[2] == '\0') {
		config_error_set("Invalid port specifier in string '%s'.", str);
		return (0);
	}
	ptr = x + 2;
	x = strchr(ptr, '+');
	if (x == NULL)
		memcpy(port_buf, ptr, sizeof(port_buf) - 1);
	else
		memcpy(port_buf, ptr, (x - ptr));

	// cert
	if (cert && x)
		cert_ptr = x + 1;

	// printf("PARSED ADDR '%s', PORT '%s'\n", addr_buf, port_buf);

	int p = atoi(port_buf);
	if (p < 0 || p > 65536) {
		config_error_set("Invalid port number '%s'", port_buf);
		return (0);
	}

	if (strcmp(addr_buf, "*") == 0) {
		if (wildcard_okay) {
			free(*addr);
			*addr = NULL;
		}
		else {
			config_error_set(
			    "Invalid address: wildcards are not allowed.");
			return (0);
		}
	} else {
		*addr = strdup(addr_buf);
	}
	*port = strdup(port_buf);
	if (cert_ptr != NULL)
		*cert = strdup(cert_ptr);

	/* printf("ADDR FINAL: '%s', '%s', '%s'\n", *addr, *port, */
	/*     cert ? *cert : ""); */

	return (1);
}

static int
config_param_host_port(char *str, char **addr, char **port, char **path)
{
	return (config_param_host_port_wildcard(str, addr, port,
		NULL, 0, path));
}


static int
config_param_val_int(char *str, int *dst, int non_negative)
{
	long  lval;
	char *ep;

	assert(str != NULL);

	errno = 0;
	lval = strtol(str, &ep, 10);

	if (*str == '\0' || *ep != '\0') {
		config_error_set("Not a number.");
		return (0);
	}
	if ((errno == ERANGE && (lval == LONG_MIN || lval == LONG_MAX)) ||
	    lval < INT_MIN || lval > INT_MAX) {
		config_error_set("Number out of range.");
		return (0);
	}
	if (non_negative && lval < 0) {
		config_error_set("Negative number.");
		return (0);
	}

	*dst = (int)lval;
	return (1);
}

static int
config_param_val_long(char *str, long *dst, int non_negative)
{
	long  lval;
	char *ep;

	assert(str != NULL);

	errno = 0;
	lval = strtol(str, &ep, 10);

	if (*str == '\0' || *ep != '\0') {
		config_error_set("Not a number.");
		return (0);
	}
	if (errno == ERANGE && (lval == LONG_MIN || lval == LONG_MAX)) {
		config_error_set("Number out of range.");
		return (0);
	}
	if (non_negative && lval < 0) {
		config_error_set("Negative number.");
		return (0);
	}

	*dst = lval;
	return (1);
}

static double
mtim2double(const struct stat *sb)
{
	double d = sb->st_mtime;

#if defined(HAVE_STRUCT_STAT_ST_MTIM)
	d += sb->st_mtim.tv_nsec * 1e-9;
#elif defined(HAVE_STRUCT_STAT_ST_MTIMESPEC)
	d += sb->st_mtimespec.tv_nsec * 1e-9;
#endif
	return (d);
}

struct cfg_cert_file *
cfg_cert_file_new(void)
{
	struct cfg_cert_file *cert;
	ALLOC_OBJ(cert, CFG_CERT_FILE_MAGIC);
	AN(cert);
	cert->ocsp_vfy = -1;
	return (cert);
}

void
cfg_cert_file_free(struct cfg_cert_file **cfptr)
{
	struct cfg_cert_file *cf;

	CHECK_OBJ_NOTNULL(*cfptr, CFG_CERT_FILE_MAGIC);
	cf = *cfptr;
	free(cf->filename);
	free(cf->ocspfn);
	FREE_OBJ(cf);
	*cfptr = NULL;
}

int
cfg_cert_vfy(struct cfg_cert_file *cf)
{
	struct stat st;
	double d;

	CHECK_OBJ_NOTNULL(cf, CFG_CERT_FILE_MAGIC);
	AN(cf->filename);

	if (cf->filename == NULL || strlen(cf->filename) <= 0)
		return (0);

	if (stat(cf->filename, &st) != 0) {
		config_error_set("Unable to stat x509 "
		    "certificate PEM file '%s': %s", cf->filename,
		    strerror(errno));
		return (0);
	}
	if (!S_ISREG(st.st_mode)) {
		config_error_set("Invalid x509 certificate "
		    "PEM file '%s': Not a file.", cf->filename);
		return (0);
	}
	cf->mtim = mtim2double(&st);

	if (cf->ocspfn != NULL) {
		if (stat(cf->ocspfn, &st) == -1) {
			config_error_set("Unable to stat OCSP "
			    "stapling file '%s': %s", cf->ocspfn,
			    strerror(errno));
			return (0);
		}
		if (!S_ISREG(st.st_mode)) {
			config_error_set("Invalid OCSP stapling file "
			    "'%s': Not a file.", cf->ocspfn);
			return (0);
		}
		cf->ocsp_mtim = mtim2double(&st);
	}

	if (cf->priv_key_filename != NULL &&
	    strlen(cf->priv_key_filename) > 0) {

		if (stat(cf->priv_key_filename, &st) != 0) {
			config_error_set("Unable to stat private keyfile "
			    "'%s': %s", cf->priv_key_filename,
			    strerror(errno));
			return (0);
		}
		if (!S_ISREG(st.st_mode)) {
			config_error_set("Invalid private keyfile "
			    "'%s': Not a file.", cf->priv_key_filename);
			return (0);
		}

		d = mtim2double(&st);
		cf->mtim = cf->mtim > d ? cf->mtim : d;
	}
	return (1);
}

void
cfg_cert_add(struct cfg_cert_file *cf, struct cfg_cert_file **dst)
{
	CHECK_OBJ_NOTNULL(cf, CFG_CERT_FILE_MAGIC);
	AN(dst);
	CHECK_OBJ_ORNULL(*dst, CFG_CERT_FILE_MAGIC);
	HASH_ADD_KEYPTR(hh, *dst, cf->filename, strlen(cf->filename), cf);
}

#ifdef USE_SHARED_CACHE
/* Parse mcast and ttl options */
static int
config_param_shcupd_mcastif(char *str, char **iface, char **ttl)
{
	char buf[150];
	char *sp;

	if (strlen(str) >= sizeof buf) {
		config_error_set("Invalid option for IFACE[,TTL]");
		return (0);
	}

	sp = strchr(str, ',');
	if (!sp) {
		if (!strcmp(str, "*"))
			*iface = NULL;
		else
			*iface = strdup(str);
		*ttl = NULL;
		return (1);
	}
	else if (!strncmp(str, "*", sp - str))
		*iface = NULL;
	else {
		*sp = 0;
		*iface = strdup(str);
	}
	*ttl = strdup(sp + 1);

	return (1);
}

static int
config_param_shcupd_peer(char *str, hitch_config *cfg)
{
	if (cfg == NULL) {
		config_error_set("Configuration pointer is NULL.");
		return (0);
	}

	// parse result
	int r = 1;

	// find place for new peer
	int offset = 0;
	int i = 0;
	for (i = 0; i < MAX_SHCUPD_PEERS; i++) {
		if (cfg->SHCUPD_PEERS[i].ip == NULL &&
		    cfg->SHCUPD_PEERS[i].port == NULL) {
			offset = i;
			break;
		}
	}
	if (i >= MAX_SHCUPD_PEERS) {
		config_error_set(
		    "Reached maximum number of shared cache update peers (%d).",
		    MAX_SHCUPD_PEERS
		);
		return (0);
	}

	// create place for new peer
	char *addr = malloc(ADDR_LEN);
	if (addr == NULL) {
		config_error_set(
		    "Unable to allocate memory for new shared cache update peer address: %s",
		    strerror(errno)
		);
		r = 0;
		goto outta_parse_peer;
	}
	memset(addr, '\0', ADDR_LEN);
	char *port = malloc(PORT_LEN);
	if (port == NULL) {
		config_error_set(
		    "Unable to allocate memory for new shared cache update peer port: %s",
		    strerror(errno)
		);
		r = 0;
		goto outta_parse_peer;
	}
	memset(port, '\0', PORT_LEN);

	// try to parse address
	if (! config_param_host_port(str, &addr, &port, NULL)) {
		r = 0;
		goto outta_parse_peer;
	}

	outta_parse_peer:

	if (! r) {
		free(addr);
		free(port);
	} else {
		cfg->SHCUPD_PEERS[offset].ip = addr;
		cfg->SHCUPD_PEERS[offset].port = port;
	}

	return (r);
}

#endif /* USE_SHARED_CACHE */

static int
check_frontend_uniqueness(struct front_arg *cur_fa, hitch_config *cfg)
{
	struct front_arg *fa, *fatmp;
	int retval = 1;

	HASH_ITER(hh, cfg->LISTEN_ARGS, fa, fatmp) {
		if (cur_fa->ip == NULL && fa->ip == NULL &&
		    strcmp(cur_fa->port, fa->port) == 0) {
			retval = 0;
			break;
		}
		else if (cur_fa->ip == NULL || fa->ip == NULL)
			continue;
		else if (strcmp(cur_fa->ip, fa->ip) == 0 &&
			 strcmp(cur_fa->port, fa->port) == 0) {
			retval = 0;
			break;
		}
	}

	if (retval == 0) {
		config_error_set("Redundant frontend "
		    "(matching IP and port) definition: '%s:%s'.",
		    fa->ip, fa->port);
	}

	return(retval);
}

int
front_arg_add(hitch_config *cfg, struct front_arg *fa)
{
	struct vsb pspec;

	CHECK_OBJ_NOTNULL(fa, FRONT_ARG_MAGIC);
	if (cfg->LISTEN_DEFAULT != NULL) {
		/* drop default listen arg. */
		struct front_arg *def = NULL;
		HASH_FIND_STR(cfg->LISTEN_ARGS, "default", def);
		AN(def);
		HASH_DEL(cfg->LISTEN_ARGS, def);
		free(def->ip);
		free(def->port);
		free(def->pspec);
		FREE_OBJ(def);
		cfg->LISTEN_DEFAULT = NULL;
	}

	VSB_new(&pspec, NULL, 0, VSB_AUTOEXTEND);
	VSB_printf(&pspec, "[%s]:%s", fa->ip, fa->port);
	VSB_finish(&pspec);
	fa->pspec = VSB_data(&pspec);

	if (fa->port == NULL) {
		config_error_set("No port number specified "
		    "for frontend '%s'", fa->pspec);
		return (0);
	}

	if (check_frontend_uniqueness(fa, cfg) == 0)
		return (0);

	HASH_ADD_KEYPTR(hh, cfg->LISTEN_ARGS, fa->pspec,
	    strlen(fa->pspec), fa);

	if (fa->match_global_certs == -1) {
		if (HASH_CNT(hh, fa->certs) == 0)
			fa->match_global_certs = 1;
		else
			fa->match_global_certs = 0;
	} else {
		if (HASH_CNT(hh, fa->certs) == 0
		    && fa->match_global_certs == 0) {
			config_error_set("No certificate configured "
			    "for frontend '%s'", fa->pspec);
			return (0);
		}
	}

	return (1);
}

int
config_param_validate(const char *k, char *v, hitch_config *cfg,
    char *file, int line)
{
	int r = 1;
	struct stat st;

	assert(k != NULL);
	assert(v != NULL);
	assert(strlen(k) >= 2);

	if (strcmp(k, "tls") == 0) {
		cfg->SELECTED_TLS_PROTOS = TLS_OPTION_PROTOS;
	} else if (strcmp(k, "ssl") == 0) {
		cfg->SELECTED_TLS_PROTOS = SSL_OPTION_PROTOS;
	} else if (strcmp(k, CFG_CIPHERS) == 0) {
		if (strlen(v) > 0) {
			config_assign_str(&cfg->CIPHERS_TLSv12, v);
		}
	} else if (strcmp(k, CFG_SSL_ENGINE) == 0) {
		if (strlen(v) > 0) {
			config_assign_str(&cfg->ENGINE, v);
		}
	} else if (strcmp(k, CFG_PREFER_SERVER_CIPHERS) == 0) {
		r = config_param_val_bool(v, &cfg->PREFER_SERVER_CIPHERS);
	} else if (strcmp(k, CFG_FRONTEND) == 0) {
		struct front_arg *fa;
		struct cfg_cert_file *cert;
		char *certfile = NULL;

		fa = front_arg_new();
		r = config_param_host_port_wildcard(v,
		    &fa->ip, &fa->port, &certfile, 1, NULL);
		if (r != 0) {
			if (certfile != NULL) {
				cert = cfg_cert_file_new();
				config_assign_str(&cert->filename, certfile);
				r = cfg_cert_vfy(cert);
				if (r != 0)
					cfg_cert_add(cert, &fa->certs);
				else
					cfg_cert_file_free(&cert);
				free(certfile);
			}
			if (r != 0)
				r = front_arg_add(cfg, fa);
			else
				FREE_OBJ(fa);
		}
	} else if (strcmp(k, CFG_BACKEND) == 0) {
		free(cfg->BACK_PORT);
		free(cfg->BACK_IP);
		free(cfg->BACK_PATH);
		r = config_param_host_port(v, &cfg->BACK_IP, &cfg->BACK_PORT,
		    &cfg->BACK_PATH);
	} else if (strcmp(k, CFG_WORKERS) == 0) {
		r = config_param_val_long(v, &cfg->NCORES, 1);
	} else if (strcmp(k, CFG_BACKLOG) == 0) {
		r = config_param_val_int(v, &cfg->BACKLOG, 0);
	} else if (strcmp(k, CFG_KEEPALIVE) == 0) {
		r = config_param_val_int(v, &cfg->TCP_KEEPALIVE_TIME, 1);
	} else if (strcmp(k, CFG_BACKEND_REFRESH) == 0) {
		r = config_param_val_int(v, &cfg->BACKEND_REFRESH_TIME, 1);
	}
#ifdef USE_SHARED_CACHE
	else if (strcmp(k, CFG_SHARED_CACHE) == 0) {
		r = config_param_val_int(v, &cfg->SHARED_CACHE, 1);
	} else if (strcmp(k, CFG_SHARED_CACHE_LISTEN) == 0) {
		if (strlen(v) > 0)
			r = config_param_host_port_wildcard(v, &cfg->SHCUPD_IP,
			    &cfg->SHCUPD_PORT, NULL, 1, NULL);
	} else if (strcmp(k, CFG_SHARED_CACHE_PEER) == 0) {
		r = config_param_shcupd_peer(v, cfg);
	} else if (strcmp(k, CFG_SHARED_CACHE_MCASTIF) == 0) {
		r = config_param_shcupd_mcastif(v, &cfg->SHCUPD_MCASTIF,
		    &cfg->SHCUPD_MCASTTTL);
	}
#endif
	else if (strcmp(k, CFG_CHROOT) == 0) {
		if (strlen(v) > 0) {
			// check directory
			if (stat(v, &st) != 0) {
				config_error_set("Unable to stat directory"
				    " '%s': %s'.",v,strerror(errno));
				r = 0;
			} else {
				if (! S_ISDIR(st.st_mode)) {
					config_error_set("Bad chroot directory "
					    "'%s': Not a directory", v);
					r = 0;
				} else {
					config_assign_str(&cfg->CHROOT, v);
				}
			}
		}
	} else if (strcmp(k, CFG_USER) == 0) {
		if (strlen(v) > 0) {
			struct passwd *passwd;
			passwd = getpwnam(v);
			if (!passwd) {
				config_error_set("Invalid user '%s'.", v);
				r = 0;
			} else {
				cfg->UID = passwd->pw_uid;
				cfg->GID = passwd->pw_gid;
			}
		}
	} else if (strcmp(k, CFG_GROUP) == 0) {
		if (strlen(v) > 0) {
			struct group *grp;
			grp = getgrnam(v);
			if (!grp) {
				config_error_set("Invalid group '%s'.", v);
				r = 0;
			} else {
				cfg->GID = grp->gr_gid;
			}
		}
	} else if (strcmp(k, CFG_QUIET) == 0) {
		int b;
		r = config_param_val_bool(v, &b);
		if (b)
			cfg->LOG_LEVEL = 0;
		else
			cfg->LOG_LEVEL = 1;
	} else if (strcmp(k, CFG_LOG_LEVEL) == 0) {
		r = config_param_val_int(v, &cfg->LOG_LEVEL, 1);
	} else if (strcmp(k, CFG_LOG_FILENAME) == 0) {
		if (strlen(v) > 0) {
			config_assign_str(&cfg->LOG_FILENAME, v);
		}
	} else if (strcmp(k, CFG_SYSLOG) == 0) {
		r = config_param_val_bool(v, &cfg->SYSLOG);
	} else if (strcmp(k, CFG_SYSLOG_FACILITY) == 0) {
		int facility = -1;
		r = 1;
#define SYSLOG_FAC(m, s)				\
		if (!strcmp(v, s))			\
			facility = m;
#include "sysl_tbl.h"
#undef SYSLOG_FAC
		if (facility != -1)
			cfg->SYSLOG_FACILITY = facility;
		else {
			config_error_set("Invalid facility '%s'.", v);
			r = 0;
		}
	} else if (strcmp(k, CFG_DAEMON) == 0) {
		r = config_param_val_bool(v, &cfg->DAEMONIZE);
	} else if (strcmp(k, CFG_WRITE_IP) == 0) {
		r = config_param_val_bool(v, &cfg->WRITE_IP_OCTET);
	} else if (strcmp(k, CFG_WRITE_PROXY) == 0) {
		r = config_param_val_bool(v, &cfg->WRITE_PROXY_LINE_V2);
	} else if (strcmp(k, CFG_WRITE_PROXY_V1) == 0) {
		r = config_param_val_bool(v, &cfg->WRITE_PROXY_LINE_V1);
	} else if (strcmp(k, CFG_WRITE_PROXY_V2) == 0) {
		r = config_param_val_bool(v, &cfg->WRITE_PROXY_LINE_V2);
	} else if (strcmp(k, CFG_PROXY_PROXY) == 0) {
		r = config_param_val_bool(v, &cfg->PROXY_PROXY_LINE);
	} else if (strcmp(k, CFG_ALPN_PROTOS) == 0) {
		if (strlen(v) > 0) {
			config_assign_str(&cfg->ALPN_PROTOS, v);
		}
	} else if (strcmp(k, CFG_PEM_FILE) == 0) {
		struct cfg_cert_file *cert;
		cert = cfg_cert_file_new();
		config_assign_str(&cert->filename, v);
		r = cfg_cert_vfy(cert);
		if (r != 0) {
			if (cfg->CERT_DEFAULT != NULL) {
				struct cfg_cert_file *tmp = cfg->CERT_DEFAULT;
				cfg_cert_add(tmp, &cfg->CERT_FILES);
			}
			cfg->CERT_DEFAULT = cert;
		} else
			cfg_cert_file_free(&cert);
	} else if (strcmp(k, CFG_BACKEND_CONNECT_TIMEOUT) == 0) {
		r = config_param_val_int(v, &cfg->BACKEND_CONNECT_TIMEOUT, 1);
	} else if (strcmp(k, CFG_SSL_HANDSHAKE_TIMEOUT) == 0) {
		r = config_param_val_int(v, &cfg->SSL_HANDSHAKE_TIMEOUT, 1);
	} else if (strcmp(k, CFG_RECV_BUFSIZE) == 0) {
		r = config_param_val_int(v, &cfg->RECV_BUFSIZE, 1);
	} else if (strcmp(k, CFG_SEND_BUFSIZE) == 0) {
		r = config_param_val_int(v, &cfg->SEND_BUFSIZE, 1);
	} else if (strcmp(k, CFG_PIDFILE) == 0) {
		if (strlen(v) > 0) {
			config_assign_str(&cfg->PIDFILE, v);
		}
	} else if (strcmp(k, CFG_RING_SLOTS) == 0) {
		r = config_param_val_int(v, &cfg->RING_SLOTS, 1);
	} else if (strcmp(k, CFG_RING_DATA_LEN) == 0) {
		r = config_param_val_int(v, &cfg->RING_DATA_LEN, 1);
	} else if (strcmp(k, CFG_SNI_NOMATCH_ABORT) == 0) {
		r = config_param_val_bool(v, &cfg->SNI_NOMATCH_ABORT);
	} else if (strcmp(k, CFG_OCSP_DIR) == 0) {
		config_assign_str(&cfg->OCSP_DIR, v);
#ifdef TCP_FASTOPEN_WORKS
	} else if (strcmp(k, CFG_TFO) == 0) {
		config_param_val_bool(v, &cfg->TFO);
#endif
	} else if (strcmp(k, CFG_TLS_PROTOS) == 0) {
		cfg->SELECTED_TLS_PROTOS = 0;
#define TLS_PROTO(u, i, s)				\
		if (strcasestr(v, s))			\
			cfg->SELECTED_TLS_PROTOS |= i;
#include "tls_proto_tbl.h"
		if (cfg->SELECTED_TLS_PROTOS == 0) {
			config_error_set("Invalid 'tls-protos' option '%s'", v);
			return (1);
		}
	} else if (strcmp(k, CFG_DBG_LISTEN) == 0) {
		config_assign_str(&cfg->DEBUG_LISTEN_ADDR, v);
	} else {
		fprintf(
			stderr,
			"Ignoring unknown configuration key '%s' in configuration file '%s', line %d\n",
			k, file, line
		);
	}

	if (!r) {
		if (file != NULL)
			config_error_set("Error in configuration file '%s', "
			    "line %d: %s\n", file, line, config_error_get());
		else
			config_error_set("Invalid parameter '%s': %s", k,
			    config_error_get());
		return (1);
	}

	return (0);
}

static int
config_file_parse(char *file, hitch_config *cfg)
{
	FILE *fp = NULL;
	int r = 0;

	AN(cfg);

	// should we read stdin?
	if (file == NULL || strlen(file) < 1 || strcmp(file, "-") == 0)
		fp = stdin;
	else
		fp = fopen(file, "r");

	if (fp == NULL) {
		config_error_set("Unable to open configuration file '%s': %s\n",
		    file, strerror(errno));
		return (1);
	}

	yyin = fp;
	do {
		if (yyparse(cfg) != 0) {
			r = 1;
			break;
		}
	} while (!feof(yyin));

	fclose(fp);
	return (r);
}

static char *
config_disp_str(char *str)
{
	return ((str == NULL) ? "" : str);
}

static char *
config_disp_bool(int v)
{
	return ((v > 0) ? CFG_BOOL_ON : "off");
}

static char *
config_disp_uid(uid_t uid)
{
	memset(tmp_buf, '\0', sizeof(tmp_buf));
	if (uid == 0 && geteuid() != 0)
		return (tmp_buf);
	struct passwd *pw = getpwuid(uid);
	if (pw) {
		strncpy(tmp_buf, pw->pw_name, sizeof(tmp_buf));
		tmp_buf[sizeof(tmp_buf) - 1] = '\0';
	}
	return (tmp_buf);
}

static char *
config_disp_gid (gid_t gid)
{
	memset(tmp_buf, '\0', sizeof(tmp_buf));
	if (gid == 0 && geteuid() != 0)
		return (tmp_buf);
	struct group *gr = getgrgid(gid);
	if (gr) {
		strncpy(tmp_buf, gr->gr_name, sizeof(tmp_buf));
		tmp_buf[sizeof(tmp_buf) - 1] = '\0';
	}
	return (tmp_buf);
}

static const char *
config_disp_hostport(char *host, char *port)
{
	memset(tmp_buf, '\0', sizeof(tmp_buf));
	if (host == NULL && port == NULL)
		return ("");

	strcat(tmp_buf, "[");
	if (host == NULL)
		strcat(tmp_buf, "*");
	else
		strncat(tmp_buf, host, 40);
	strcat(tmp_buf, "]:");
	strncat(tmp_buf, port, 5);
	return (tmp_buf);
}

static const char *
config_disp_log_facility (int facility)
{
	switch (facility)
	{
#define SYSLOG_FAC(m, s)			\
		case m:				\
			return (s);
#include "sysl_tbl.h"
#undef SYSLOG_FAC
		default:
			return ("UNKNOWN");
	}
}

int
config_scan_pem_dir(char *pemdir, hitch_config *cfg)
{
	int n, i, plen;
	int retval = 0;
	struct dirent **d;
	struct stat st;

	n = scandir(pemdir, &d, NULL, alphasort);
	if (n < 0) {
		config_error_set("Unable to open directory '%s': %s", pemdir,
		    strerror(errno));
		return (1);
	}
	for (i = 0; i < n; i++) {
		struct cfg_cert_file *cert;
		char *fpath;

		plen = strlen(pemdir) + strlen(d[i]->d_name) + 1;

		if (cfg->PEM_DIR_GLOB != NULL) {
			if (fnmatch(cfg->PEM_DIR_GLOB, d[i]->d_name, 0))
				continue;
		}
		if (d[i]->d_type != DT_UNKNOWN && d[i]->d_type != DT_REG)
			continue;

		fpath = malloc(plen);
		AN(fpath);

		if (snprintf(fpath, plen, "%s%s", pemdir, d[i]->d_name) < 0) {
			config_error_set("An error occurred while "
			    "combining path");
			free(fpath);
			retval = 1;
			break;
		}

		if (d[i]->d_type == DT_UNKNOWN) {
			/* The underlying filesystem does not support d_type. */
			if (lstat(fpath, &st) < 0) {
				fprintf(stderr, "Warning: unable to stat '%s': %s. Skipping.\n",
				    fpath, strerror(errno));
				free(fpath);
				continue;
			}
			if (!S_ISREG(st.st_mode)) {
				free(fpath);
				continue;
			}
		}

		cert = cfg_cert_file_new();
		config_assign_str(&cert->filename, fpath);
		free(fpath);

		int r = cfg_cert_vfy(cert);
		if (r != 0) {
			/* If no default has been set, use the first
			 * match according to alphasort  */
			if (cfg->CERT_DEFAULT == NULL)
				cfg->CERT_DEFAULT = cert;
			else
				cfg_cert_add(cert, &cfg->CERT_FILES);
		} else {
			cfg_cert_file_free(&cert);
		}
	}

	for (i = 0; i < n; i++)
		free(d[i]);
	free(d);

	return (retval);
}

void
config_print_usage_fd(char *prog, FILE *out)
{
	hitch_config *cfg;

	cfg = config_new();
	AN(cfg);

	if (out == NULL)
		out = stderr;
	fprintf(out, "Usage: %s [OPTIONS] PEM\n\n", basename(prog));
	fprintf(out, "This is hitch, The Scalable TLS Unwrapping Daemon.\n\n");
	fprintf(out, "CONFIGURATION:\n");
	fprintf(out, "\n");
	fprintf(out, "\t--config=FILE\n");
	fprintf(out, "\t\tLoad configuration from specified file.\n");
	fprintf(out, "\n");
	fprintf(out, "ENCRYPTION METHODS:\n");
	fprintf(out, "\n");
	fprintf(out, "\t--tls-protos=LIST\n");
	fprintf(out, "\t\tSpecifies which SSL/TLS protocols to use.\n");
	fprintf(out, "\t\tAvailable tokens are SSLv3, TLSv1.0, TLSv1.1\n");
	fprintf(out, "\t\tTLSv1.2 and TLSv1.3. (Default: \"TLSv1.2 TLSv1.3\")\n");
	fprintf(out, "\t-c  --ciphers=SUITE\n");
	fprintf(out, "\t\tSets allowed ciphers (Default: \"%s\")\n",
	    config_disp_str(cfg->CIPHERS_TLSv12));
	fprintf(out, "\t-e  --ssl-engine=NAME\n");
	fprintf(out, "\t\tSets OpenSSL engine (Default: \"%s\")\n",
	    config_disp_str(cfg->ENGINE));
	fprintf(out, "\t-O  --prefer-server-ciphers[=on|off]\n");
	fprintf(out, "\t\tPrefer server list order (Default: \"%s\")\n",
	    config_disp_bool(cfg->PREFER_SERVER_CIPHERS));
	fprintf(out, "\n");
	fprintf(out, "SOCKET:\n");
	fprintf(out, "\n");
	fprintf(out, "\t--client\n");
	fprintf(out, "\t\tEnable client proxy mode\n");
	fprintf(out, "\t-b  --backend=[HOST]:PORT\n");
	fprintf(out, "\t\tBackend endpoint (default is \"%s\")\n",
	    config_disp_hostport(cfg->BACK_IP, cfg->BACK_PORT));
	fprintf(out,
	    "\t\tThe -b argument can also take a UNIX domain socket path\n");
	fprintf(out, "\t\tE.g. --backend=\"/path/to/sock\"\n");
	fprintf(out, "\t-f  --frontend=[HOST]:PORT[+CERT]\n");
	fprintf(out, "\t\tFrontend listen endpoint (default is \"%s\")\n",
	    config_disp_hostport(cfg->LISTEN_DEFAULT->ip,
		cfg->LISTEN_DEFAULT->port));
	fprintf(out,
	    "\t\t(Note: brackets are mandatory in endpoint specifiers.)\n");
	fprintf(out, "\t--recv-bufsize=SIZE\n");
	fprintf(out, "\t\tReceive buffer size on client socket (Default: %d)\n",
	    cfg->RECV_BUFSIZE);
	fprintf(out, "\t--send-bufsize=SIZE\n");
	fprintf(out, "\t\tSend buffer size on client socket (Default: %d)\n",
	    cfg->SEND_BUFSIZE);

#ifdef USE_SHARED_CACHE
	fprintf(out, "\n");
	fprintf(out, "\t-U  --shared-cache-listen=[HOST]:PORT\n");
	fprintf(out, "\t\tAccept cache updates on UDP (Default: \"%s\")\n",
	    config_disp_hostport(cfg->SHCUPD_IP, cfg->SHCUPD_PORT));
	fprintf(out,
	    "\t\tNOTE: This option requires enabled SSL session cache.\n");
	fprintf(out, "\t-P  --shared-cache-peer=[HOST]:PORT\n");
	fprintf(out, "\t\tSend cache updates to specified peer\n");
	fprintf(out,
	    "\t\tNOTE: This option can be specified multiple times.\n");
	fprintf(out, "\t-M  --shared-cache-if=IFACE[,TTL]\n");
	fprintf(out,
	    "\t\tForce iface and ttl to receive and send multicast updates\n");
#endif

	fprintf(out, "\n");
	fprintf(out, "PERFORMANCE:\n");
	fprintf(out, "\n");
	fprintf(out, "\t-n  --workers=NUM\n");
	fprintf(out, "\t\tNumber of worker processes (Default: %ld)\n",
	    cfg->NCORES);
	fprintf(out, "\t-B  --backlog=NUM\n");
	fprintf(out, "\t\tSet listen backlog size (Default: %d)\n", cfg->BACKLOG);
	fprintf(out, "\t-k  --keepalive=SECS\n");
	fprintf(out, "\t\tTCP keepalive on client socket (Default: %d)\n",
	    cfg->TCP_KEEPALIVE_TIME);
	fprintf(out, "\t-R  --backend-refresh=SECS\n");
	fprintf(out, "\t\tPeriodic backend IP lookup, 0 to disable (Default: %d)\n",
	    cfg->BACKEND_REFRESH_TIME);

#ifdef USE_SHARED_CACHE
	fprintf(out, "\t-C  --session-cache=NUM\n");
	fprintf(out,
	    "\t\tEnable and set SSL session cache to specified number\n");
	fprintf(out, "\t\tof sessions (Default: %d)\n", cfg->SHARED_CACHE);
#endif
#ifdef TCP_FASTOPEN_WORKS
	fprintf(out, "\t--enable-tcp-fastopen[=on|off]\n");
	fprintf(out, "\t\tEnable client-side TCP Fast Open. (Default: %s)\n",
	    config_disp_bool(cfg->TFO));
#endif
	fprintf(out, "\n");
	fprintf(out, "SECURITY:\n");
	fprintf(out, "\n");
	fprintf(out, "\t-r  --chroot=DIR\n");
	fprintf(out, "\t\tSets chroot directory (Default: \"%s\")\n",
	    config_disp_str(cfg->CHROOT));
	fprintf(out, "\t-u  --user=USER\n ");
	fprintf(out,
	    "\t\tSet uid/gid after binding the socket (Default: \"%s\")\n",
	    config_disp_uid(cfg->UID));
	fprintf(out, "\t-g  --group=GROUP\n");
	fprintf(out, "\t\tSet gid after binding the socket (Default: \"%s\")\n",
	    config_disp_gid(cfg->GID));
	fprintf(out, "\n");
	fprintf(out, "LOGGING:\n");
	fprintf(out, "\t-q  --quiet[=on|off]\n");
	fprintf(out, "\t\tBe quiet; emit only error messages "
	    "(deprecated, use 'log-level')\n");
	fprintf(out, "\t-L  --log-level=NUM\n");
	fprintf(out, "\t\tLog level. 0=silence, 1=err, 2=info/debug (Default: %d)\n",
		cfg->LOG_LEVEL);
	fprintf(out, "\t-l  --log-filename=FILE \n");
	fprintf(out,
	    "\t\tSend log message to a logfile instead of stderr/stdout\n");
	fprintf(out, "\t-s  --syslog[=on|off]   \n");
	fprintf(out,
	    "\t\tSend log message to syslog in addition to stderr/stdout\n");
	fprintf(out, "\t--syslog-facility=FACILITY\n");
	fprintf(out, "\t\tSyslog facility to use (Default: \"%s\")\n",
	    config_disp_log_facility(cfg->SYSLOG_FACILITY));
	fprintf(out, "\n");
	fprintf(out, "OTHER OPTIONS:\n");
	fprintf(out, "\t--daemon[=on|off]\n");
	fprintf(out, "\t\tFork into background and become a daemon (Default: %s)\n",
	    config_disp_bool(cfg->DAEMONIZE));
	fprintf(out, "\t--write-ip[=on|off]\n");
	fprintf(out,
	    "\t\tWrite 1 octet with the IP family followed by the IP\n");
	fprintf(out,
	    "\t\taddress in 4 (IPv4) or 16 (IPv6) octets little-endian\n");
	fprintf(out,
	    "\t\tto backend before the actual data\n");
	fprintf(out,
	    "\t\t(Default: %s)\n", config_disp_bool(cfg->WRITE_IP_OCTET));
	fprintf(out, "\t--write-proxy-v1[=on|off]\n");
	fprintf(out,
	    "\t\tWrite HAProxy's PROXY v1 (IPv4 or IPv6) protocol line\n");
	fprintf(out, "\t\tbefore actual data\n");
	fprintf(out, "\t\t(Default: %s)\n",
	    config_disp_bool(cfg->WRITE_PROXY_LINE_V1));
	fprintf(out, "\t--write-proxy-v2[=on|off]\n");
	fprintf(out, "\t\tWrite HAProxy's PROXY v2 binary (IPv4 or IPv6)\n");
	fprintf(out, "\t\t protocol line before actual data\n");
	fprintf(out, "\t\t(Default: %s)\n",
	    config_disp_bool(cfg->WRITE_PROXY_LINE_V2));
	fprintf(out, "\t--write-proxy[=on|off]\n");
	fprintf(out, "\t\tEquivalent to --write-proxy-v2. For PROXY \n");
	fprintf(out, "\t\tversion 1 use --write-proxy-v1 explicitly\n");
	fprintf(out, "\t--proxy-proxy[=on|off]\n");
	fprintf(out, "\t\tProxy HAProxy's PROXY (IPv4 or IPv6) protocol\n");
	fprintf(out, "\t\tbefore actual data (PROXYv1 and PROXYv2)\n");
	fprintf(out, "\t\t(Default: %s)\n",
	    config_disp_bool(cfg->PROXY_PROXY_LINE));
	fprintf(out, "\t--sni-nomatch-abort[=on|off]\n");
	fprintf(out, "\t\tAbort handshake when client submits an\n");
	fprintf(out, "\t\tunrecognized SNI server name\n" );
	fprintf(out, "\t\t(Default: %s)\n",
			config_disp_bool(cfg->SNI_NOMATCH_ABORT));
	fprintf(out, "\t--alpn-protos=LIST\n");
	fprintf(out, "\t\tSets the protocols for ALPN/NPN negotiation,\n");
	fprintf(out, "\t\tprovided as a list of comma-separated tokens\n");
	fprintf(out, "\t--ocsp-dir=DIR\n");
	fprintf(out, "\t\tSet OCSP staple cache directory\n");
	fprintf(out, "\t\tThis enables automated retrieval and stapling\n"
	    "\t\tof OCSP responses\n");
	fprintf(out, "\t\t(Default: \"%s\")\n", config_disp_str(cfg->OCSP_DIR));
	fprintf(out, "\n");
	fprintf(out, "\t-t  --test\n");
	fprintf(out, "\t\tTest configuration and exit\n");
	fprintf(out, "\t-p  --pidfile=FILE\n");
	fprintf(out, "\t\tPID file\n");
	fprintf(out, "\t-V  --version\n");
	fprintf(out, "\t\tPrint program version and exit\n");
	fprintf(out, "\t-h  --help\n");
	fprintf(out, "\t\tThis help message\n");

	config_destroy(cfg);
}

static void
config_print_usage(char *prog)
{
	config_print_usage_fd(prog, stdout);
}

static int
create_alpn_callback_data(hitch_config *cfg, char **error)
{
	size_t i = 1, j, l;

	AN(cfg->ALPN_PROTOS);
	l = strlen(cfg->ALPN_PROTOS);
	cfg->ALPN_PROTOS_LV = malloc(l + 1);
	AN(cfg->ALPN_PROTOS_LV);

	// first remove spaces while copying to cfg->ALPN_PROTOS_LV
	for(j = 0; j < l; j++)
		if (!isspace(cfg->ALPN_PROTOS[j])) {
			cfg->ALPN_PROTOS_LV[i] = cfg->ALPN_PROTOS[j];
			i++;
		}

	l = i - 1; // same as before iff cfg->ALPN_PROTOS has no spaces
	i = 0; // location of next "length" byte
	for(j = 1; j <= l; j++) {
		if (cfg->ALPN_PROTOS_LV[j] == ',') {
			if (i + 1 == j) {
				*error = "alpn-protos has empty proto in list";
				return (0); // failure
			}
			if (j - i > 256) {
				free(cfg->ALPN_PROTOS_LV);
				cfg->ALPN_PROTOS_LV = NULL;
				*error = "alpn protocol too long";
				return (0);
			}
			cfg->ALPN_PROTOS_LV[i] = (unsigned char)(j - i - 1);
			i = j;
		}
	}
	if (i == j) {
		// alpn-protos ends with a comma - we let it slide
		cfg->ALPN_PROTOS_LV_LEN = l;
	} else {
		if (j - i > 256) {
			free(cfg->ALPN_PROTOS_LV);
			cfg->ALPN_PROTOS_LV = NULL;
			*error = "alpn protocol too long";
			return (0);
		}
		cfg->ALPN_PROTOS_LV[i] = (unsigned char)(j - i - 1);
		cfg->ALPN_PROTOS_LV_LEN = l + 1;
	}
	return (1); // ok!
}

int
config_parse_cli(int argc, char **argv, hitch_config *cfg)
{
	static int tls = 0, ssl = 0;
	struct front_arg *fa, *fatmp;
	static int client = 0;
	int c, i;

	optind = 1;

	struct option long_options[] = {
		{ CFG_CONFIG, 1, NULL, CFG_PARAM_CFGFILE },
		{ "tls", 0, &tls, 1},
		{ "ssl", 0, &ssl, 1},
		{ "client", 0, &client, 1},
		{ CFG_CIPHERS, 1, NULL, 'c' },
		{ CFG_PREFER_SERVER_CIPHERS, 2, NULL, 'O' },
		{ CFG_BACKEND, 1, NULL, 'b' },
		{ CFG_FRONTEND, 1, NULL, 'f' },
		{ CFG_WORKERS, 1, NULL, 'n' },
		{ CFG_BACKLOG, 1, NULL, 'B' },
#ifdef USE_SHARED_CACHE
		{ CFG_SHARED_CACHE, 1, NULL, 'C' },
		{ CFG_SHARED_CACHE_LISTEN, 1, NULL, 'U' },
		{ CFG_SHARED_CACHE_PEER, 1, NULL, 'P' },
		{ CFG_SHARED_CACHE_MCASTIF, 1, NULL, 'M' },
#endif
		{ CFG_PIDFILE, 1, NULL, 'p' },
		{ CFG_KEEPALIVE, 1, NULL, 'k' },
		{ CFG_BACKEND_REFRESH, 1, NULL, 'R' },
		{ CFG_CHROOT, 1, NULL, 'r' },
		{ CFG_USER, 1, NULL, 'u' },
		{ CFG_GROUP, 1, NULL, 'g' },
		{ CFG_QUIET, 2, NULL, 'q' },
		{ CFG_LOG_FILENAME, 1, NULL, 'l' },
		{ CFG_LOG_LEVEL, 1, NULL, 'L' },
		{ CFG_SYSLOG, 2, NULL, 's' },
		{ CFG_SYSLOG_FACILITY, 1, NULL, CFG_PARAM_SYSLOG_FACILITY },
		{ CFG_SEND_BUFSIZE, 1, NULL, CFG_PARAM_SEND_BUFSIZE },
		{ CFG_RECV_BUFSIZE, 1, NULL, CFG_PARAM_RECV_BUFSIZE },
#ifdef TCP_FASTOPEN_WORKS
		{ CFG_TFO, 2, NULL, 1 },
#endif
		{ CFG_DAEMON, 2, NULL, 1 },
		{ CFG_WRITE_IP, 2, NULL, 1 },
		{ CFG_WRITE_PROXY_V1, 2, NULL, 1 },
		{ CFG_WRITE_PROXY_V2, 2, NULL, 1 },
		{ CFG_WRITE_PROXY, 2, NULL, 1 },
		{ CFG_PROXY_PROXY, 2, NULL, 1 },
		{ CFG_ALPN_PROTOS, 1, NULL, CFG_PARAM_ALPN_PROTOS },
		{ CFG_SNI_NOMATCH_ABORT, 2, NULL, 1 },
		{ CFG_OCSP_DIR, 1, NULL, 'o' },
		{ CFG_TLS_PROTOS, 1, NULL, CFG_PARAM_TLS_PROTOS },
		{ CFG_DBG_LISTEN, 1, NULL, CFG_PARAM_DBG_LISTEN },
		{ "test", 0, NULL, 't' },
		{ "version", 0, NULL, 'V' },
		{ "help", 0, NULL, 'h' },
		{ 0, 0, 0, 0 }
	};
#define SHORT_OPTS "c:e:Ob:f:n:B:l:L:C:U:p:P:M:k:r:u:g:qstVho:R:"

	if (argc == 1) {
		config_print_usage(argv[0]);
		return (1);
	}

	/* First do a pass over the args string to see if there was a
	 * config file present. If so, apply its options first in
	 * order to let them be overridden by the command line.  */
	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, SHORT_OPTS,
			long_options, &option_index);
		if (c == -1) {
			break;
		}
		else if (c == '?') {
			config_error_set("Invalid command line parameters. "
			    "Run %s --help for instructions.",
			    basename(argv[0]));
			return (1);
		}
		else if (c == CFG_PARAM_CFGFILE) {
			if (config_file_parse(optarg, cfg) != 0) {
				return (1);
			}
		}
	}

	int tls_protos_config_file = cfg->SELECTED_TLS_PROTOS;

	optind = 1;
	while (1) {
		int ret = 0;
		int option_index = 0;
		c = getopt_long(argc, argv, SHORT_OPTS,
			long_options, &option_index);

		if (c == -1)
			break;

		switch (c) {
		case 0:
			break;
		case CFG_PARAM_CFGFILE:
			/* Handled above */
			break;
#define CFG_ARG(opt, key)						\
			case opt:					\
			ret = config_param_validate(key,		\
			    optarg, cfg, NULL, 0);			\
			break;
#define CFG_BOOL(opt, key)						\
			case opt:					\
			ret = config_param_validate(key,		\
			    optarg ? optarg : CFG_BOOL_ON,		\
			    cfg, NULL, 0);				\
			break;
CFG_ARG(CFG_PARAM_SYSLOG_FACILITY, CFG_SYSLOG_FACILITY);
CFG_ARG(CFG_PARAM_SEND_BUFSIZE, CFG_SEND_BUFSIZE);
CFG_ARG(CFG_PARAM_RECV_BUFSIZE, CFG_RECV_BUFSIZE);
CFG_ARG(CFG_PARAM_ALPN_PROTOS, CFG_ALPN_PROTOS);
CFG_ARG(CFG_PARAM_TLS_PROTOS, CFG_TLS_PROTOS);
CFG_ARG(CFG_PARAM_DBG_LISTEN, CFG_DBG_LISTEN);
CFG_ARG('c', CFG_CIPHERS);
CFG_ARG('e', CFG_SSL_ENGINE);
CFG_ARG('b', CFG_BACKEND);
CFG_ARG('f', CFG_FRONTEND);
CFG_ARG('n', CFG_WORKERS);
CFG_ARG('B', CFG_BACKLOG);
#ifdef USE_SHARED_CACHE
CFG_ARG('C', CFG_SHARED_CACHE);
CFG_ARG('U', CFG_SHARED_CACHE_LISTEN);
CFG_ARG('P', CFG_SHARED_CACHE_PEER);
CFG_ARG('M', CFG_SHARED_CACHE_MCASTIF);
#endif
CFG_ARG('p', CFG_PIDFILE);
CFG_ARG('k', CFG_KEEPALIVE);
CFG_ARG('R', CFG_BACKEND_REFRESH);
CFG_ARG('r', CFG_CHROOT);
CFG_ARG('u', CFG_USER);
CFG_ARG('g', CFG_GROUP);
CFG_ARG('o', CFG_OCSP_DIR);
CFG_BOOL('O', CFG_PREFER_SERVER_CIPHERS);
CFG_BOOL('q', CFG_QUIET);
CFG_ARG('l', CFG_LOG_FILENAME);
CFG_ARG('L', CFG_LOG_LEVEL);
CFG_BOOL('s', CFG_SYSLOG);
#undef CFG_ARG
#undef CFG_BOOL
		case 1:
			assert (option_index > 0);
			if (optarg != NULL) {
				if (strcmp(optarg, "on") &&
				    strcmp(optarg, "off")) {
					config_error_set(
						"Invalid argument '%s' for option '%s': "
						    "expected one of 'on' or 'off",
						    optarg,
						    long_options[option_index].name);
					return (1);
				}
			}
			ret = config_param_validate(
				long_options[option_index].name,
				    optarg ? optarg : CFG_BOOL_ON,
				    cfg, NULL, 0);
			break;
		case 't':
			cfg->TEST = 1;
			break;
		case 'V':
			printf("%s %s\n", basename(argv[0]), VERSION);
			exit(0);
		case 'h':
			config_print_usage(argv[0]);
			exit(0);
		default:
			config_error_set("Invalid command line parameters. "
			    "Run %s --help for instructions.",
			    basename(argv[0]));
			return (1);
		}

		if (ret != 0) {
			return (1);
		}
	}

	if ((tls || ssl) && tls_protos_config_file != 0) {
		config_error_set("Deprecated options --tls and --ssl cannot be"
		    " used to override tls-protos in a config file.");
		return (1);
	}
	if (tls && ssl) {
		config_error_set("Options --tls and --ssl are mutually"
		    " exclusive.");
		return (1);
	} else {
		if (ssl)
			cfg->SELECTED_TLS_PROTOS = SSL_OPTION_PROTOS;
		else if (tls)
			cfg->SELECTED_TLS_PROTOS = TLS_OPTION_PROTOS;
	}
	if (cfg->SELECTED_TLS_PROTOS == 0)
		cfg->SELECTED_TLS_PROTOS = DEFAULT_TLS_PROTOS;

	if (client)
		cfg->PMODE = SSL_CLIENT;

	if ((!!cfg->WRITE_IP_OCTET + !!cfg->PROXY_PROXY_LINE +
		!!cfg->WRITE_PROXY_LINE_V1 + !!cfg->WRITE_PROXY_LINE_V2) >= 2) {
		config_error_set("Options --write-ip, --write-proxy-proxy,"
		    " --write-proxy-v1 and --write-proxy-v2 are"
		    " mutually exclusive.");
		return (1);
	}

	if (cfg->CLIENT_VERIFY != SSL_VERIFY_NONE &&
	    cfg->CLIENT_VERIFY_CA == NULL) {
		config_error_set("Setting 'client-verify-ca' is required when"
		    " configuring client-verify");
		return (1);
	}

	HASH_ITER(hh, cfg->LISTEN_ARGS, fa, fatmp) {
		if (fa->client_verify != -1 &&
		    fa->client_verify != SSL_VERIFY_NONE) {
			if (!fa->client_verify_ca && !cfg->CLIENT_VERIFY_CA) {
				config_error_set("No 'client-verify-ca' "
				    "configured for frontend '%s'",
				    fa->pspec);
				return (1);
			}
		}

	}


#ifdef USE_SHARED_CACHE
	if (cfg->SHCUPD_IP != NULL && ! cfg->SHARED_CACHE) {
		config_error_set("Shared cache update listener is defined,"
		    " but shared cache is disabled.");
		return (1);
	}
#endif

	/* ALPN/NPN protocol negotiation additional configuration and error
	   handling */
	if (cfg->ALPN_PROTOS != NULL) {
		char *error;
		if (!create_alpn_callback_data(cfg, &error)) {
			if (error)
				config_error_set("alpn-protos configuration"
				    " \"%s\" is bad. %s",
				    cfg->ALPN_PROTOS, error);
			else
				config_error_set("alpn-protos configuration"
				    " \"%s\" is bad. See man page for more"
				    " info.",
				    cfg->ALPN_PROTOS);
			return (1);
		}
#if defined(OPENSSL_WITH_NPN) || defined(OPENSSL_WITH_ALPN)
		/*
		if (cfg->WRITE_PROXY_LINE_V2)
			fprintf(stderr, ALPN_NPN_PREFIX_STR
			    " Negotiated protocol will be communicated to the"
			    " backend.\n");
		*/
#  ifndef OPENSSL_WITH_ALPN
		fprintf(stderr, ALPN_NPN_PREFIX_STR " Warning: Hitch has been"
		    " compiled against a version of OpenSSL without ALPN"
		    " support.\n");
#  endif
#else
		AN(cfg->ALPN_PROTOS_LV);
		int multi_proto =
		    cfg->ALPN_PROTOS_LV[0] != cfg->ALPN_PROTOS_LV_LEN - 1;
		/* No support for ALPN / NPN support in OpenSSL */
		if (multi_proto ||
		    0 != strncmp((char *)cfg->ALPN_PROTOS_LV, "\x8http/1.1", 9)) {
			config_error_set("This is compiled against OpenSSL version"
			    " %lx, which does not have NPN or ALPN support,"
			    " yet alpn-protos has been set to %s.",
			    OPENSSL_VERSION_NUMBER, cfg->ALPN_PROTOS);
			return (1);
		}
		else
			fprintf(stderr, "This is compiled against OpenSSL version"
			    " %lx, which does not have NPN or ALPN support."
			    " alpn-protos setting \"http/1.1\" will be ignored.\n",
			    OPENSSL_VERSION_NUMBER);
#endif
	}

	// Any arguments left are presumed to be PEM files
	argc -= optind;
	argv += optind;
	for (i = 0; i < argc; i++) {
		if (config_param_validate(CFG_PEM_FILE, argv[i], cfg, NULL, 0)) {
			return (1);
		}
	}

	if (cfg->PEM_DIR != NULL) {
		if (config_scan_pem_dir(cfg->PEM_DIR, cfg))
			return (1);
	}

	if (cfg->PMODE == SSL_SERVER && cfg->CERT_DEFAULT == NULL) {
		HASH_ITER(hh, cfg->LISTEN_ARGS, fa, fatmp)
			if (HASH_CNT(hh, fa->certs) == 0) {
				config_error_set("No x509 certificate PEM file "
				    "specified for frontend '%s'!", fa->pspec);
				return (1);
			}
	}

	if (cfg->OCSP_DIR != NULL) {
		struct stat sb;

		if (stat(cfg->OCSP_DIR, &sb) != 0) {
			fprintf(stderr,
			    "{ocsp} Warning: Unable to stat directory '%s': %s'."
			    " OCSP stapling will be disabled.\n",
			    cfg->OCSP_DIR, strerror(errno));
			free(cfg->OCSP_DIR);
			cfg->OCSP_DIR = NULL;
		} else {
			if (!S_ISDIR(sb.st_mode)) {
				fprintf(stderr, "{ocsp} Bad ocsp-dir "
				    "'%s': Not a directory."
				    " OCSP stapling will be disabled.\n", cfg->OCSP_DIR);
				free(cfg->OCSP_DIR);
				cfg->OCSP_DIR = NULL;
			}
		}
	}

	return (0);
}
