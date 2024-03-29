/*
 * SSL/TLS transport layer over SOCK_STREAM sockets
 *
 * Copyright (C) 2012 EXCELIANCE, Emeric Brun <ebrun@exceliance.fr>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * Acknowledgement:
 *   We'd like to specially thank the Stud project authors for a very clean
 *   and well documented code which helped us understand how the OpenSSL API
 *   ought to be used in non-blocking mode. This is one difficult part which
 *   is not easy to get from the OpenSSL doc, and reading the Stud code made
 *   it much more obvious than the examples in the OpenSSL package. Keep up
 *   the good works, guys !
 *
 *   Stud is an extremely efficient and scalable SSL/TLS proxy which combines
 *   particularly well with haproxy. For more info about this project, visit :
 *       https://github.com/bumptech/stud
 *
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <netinet/tcp.h>

#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/x509.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include <common/buffer.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <common/errors.h>
#include <common/standard.h>
#include <common/ticks.h>
#include <common/time.h>

#include <ebsttree.h>

#include <types/global.h>
#include <types/ssl_sock.h>

#include <proto/acl.h>
#include <proto/arg.h>
#include <proto/connection.h>
#include <proto/fd.h>
#include <proto/freq_ctr.h>
#include <proto/frontend.h>
#include <proto/listener.h>
#include <proto/pattern.h>
#include <proto/server.h>
#include <proto/log.h>
#include <proto/proxy.h>
#include <proto/shctx.h>
#include <proto/ssl_sock.h>
#include <proto/task.h>

#define SSL_SOCK_ST_FL_VERIFY_DONE  0x00000001
/* bits 0xFFFF0000 are reserved to store verify errors */

/* Verify errors macros */
#define SSL_SOCK_CA_ERROR_TO_ST(e) (((e > 63) ? 63 : e) << (16))
#define SSL_SOCK_CAEDEPTH_TO_ST(d) (((d > 15) ? 15 : d) << (6+16))
#define SSL_SOCK_CRTERROR_TO_ST(e) (((e > 63) ? 63 : e) << (4+6+16))

#define SSL_SOCK_ST_TO_CA_ERROR(s) ((s >> (16)) & 63)
#define SSL_SOCK_ST_TO_CAEDEPTH(s) ((s >> (6+16)) & 15)
#define SSL_SOCK_ST_TO_CRTERROR(s) ((s >> (4+6+16)) & 63)

static int sslconns = 0;

void ssl_sock_infocbk(const SSL *ssl, int where, int ret)
{
	struct connection *conn = (struct connection *)SSL_get_app_data(ssl);
	(void)ret; /* shut gcc stupid warning */

	if (where & SSL_CB_HANDSHAKE_START) {
		/* Disable renegotiation (CVE-2009-3555) */
		if (conn->flags & CO_FL_CONNECTED) {
			conn->flags |= CO_FL_ERROR;
			conn->err_code = CO_ER_SSL_RENEG;
		}
	}
}

/* Callback is called for each certificate of the chain during a verify
   ok is set to 1 if preverify detect no error on current certificate.
   Returns 0 to break the handshake, 1 otherwise. */
int ssl_sock_bind_verifycbk(int ok, X509_STORE_CTX *x_store)
{
	SSL *ssl;
	struct connection *conn;
	int err, depth;

	ssl = X509_STORE_CTX_get_ex_data(x_store, SSL_get_ex_data_X509_STORE_CTX_idx());
	conn = (struct connection *)SSL_get_app_data(ssl);

	conn->xprt_st |= SSL_SOCK_ST_FL_VERIFY_DONE;

	if (ok) /* no errors */
		return ok;

	depth = X509_STORE_CTX_get_error_depth(x_store);
	err = X509_STORE_CTX_get_error(x_store);

	/* check if CA error needs to be ignored */
	if (depth > 0) {
		if (!SSL_SOCK_ST_TO_CA_ERROR(conn->xprt_st)) {
			conn->xprt_st |= SSL_SOCK_CA_ERROR_TO_ST(err);
			conn->xprt_st |= SSL_SOCK_CAEDEPTH_TO_ST(depth);
		}

		if (objt_listener(conn->target)->bind_conf->ca_ignerr & (1ULL << err)) {
			ERR_clear_error();
			return 1;
		}

		conn->err_code = CO_ER_SSL_CA_FAIL;
		return 0;
	}

	if (!SSL_SOCK_ST_TO_CRTERROR(conn->xprt_st))
		conn->xprt_st |= SSL_SOCK_CRTERROR_TO_ST(err);

	/* check if certificate error needs to be ignored */
	if (objt_listener(conn->target)->bind_conf->crt_ignerr & (1ULL << err)) {
		ERR_clear_error();
		return 1;
	}

	conn->err_code = CO_ER_SSL_CRT_FAIL;
	return 0;
}

#ifdef OPENSSL_NPN_NEGOTIATED
/* This callback is used so that the server advertises the list of
 * negociable protocols for NPN.
 */
static int ssl_sock_advertise_npn_protos(SSL *s, const unsigned char **data,
                                         unsigned int *len, void *arg)
{
	struct bind_conf *conf = arg;

	*data = (const unsigned char *)conf->npn_str;
	*len = conf->npn_len;
	return SSL_TLSEXT_ERR_OK;
}
#endif

#ifdef OPENSSL_ALPN_NEGOTIATED
/* This callback is used so that the server advertises the list of
 * negociable protocols for ALPN.
 */
static int ssl_sock_advertise_alpn_protos(SSL *s, const unsigned char **data,
                                          unsigned int *len, void *arg)
{
	struct bind_conf *conf = arg;

	*data = (const unsigned char *)conf->alpn_str;
	*len = conf->alpn_len;
	return SSL_TLSEXT_ERR_OK;
}
#endif

#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
/* Sets the SSL ctx of <ssl> to match the advertised server name. Returns a
 * warning when no match is found, which implies the default (first) cert
 * will keep being used.
 */
static int ssl_sock_switchctx_cbk(SSL *ssl, int *al, struct bind_conf *s)
{
	const char *servername;
	const char *wildp = NULL;
	struct ebmb_node *node, *n;
	int i;
	(void)al; /* shut gcc stupid warning */

	servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
	if (!servername) {
		return (s->strict_sni ?
			SSL_TLSEXT_ERR_ALERT_FATAL :
			SSL_TLSEXT_ERR_NOACK);
	}

	for (i = 0; i < trash.size; i++) {
		if (!servername[i])
			break;
		trash.str[i] = tolower(servername[i]);
		if (!wildp && (trash.str[i] == '.'))
			wildp = &trash.str[i];
	}
	trash.str[i] = 0;

	/* lookup in full qualified names */
	node = ebst_lookup(&s->sni_ctx, trash.str);

	/* lookup a not neg filter */
	for (n = node; n; n = ebmb_next_dup(n)) {
		if (!container_of(n, struct sni_ctx, name)->neg) {
			node = n;
			break;
		}
	}
	if (!node && wildp) {
		/* lookup in wildcards names */
		node = ebst_lookup(&s->sni_w_ctx, wildp);
	}
	if (!node || container_of(node, struct sni_ctx, name)->neg) {
		return (s->strict_sni ?
			SSL_TLSEXT_ERR_ALERT_FATAL :
			SSL_TLSEXT_ERR_ALERT_WARNING);
	}

	/* switch ctx */
	SSL_set_SSL_CTX(ssl, container_of(node, struct sni_ctx, name)->ctx);
	return SSL_TLSEXT_ERR_OK;
}
#endif /* SSL_CTRL_SET_TLSEXT_HOSTNAME */

#ifndef OPENSSL_NO_DH
/* Loads Diffie-Hellman parameter from a file. Returns 1 if loaded, else -1
   if an error occured, and 0 if parameter not found. */
int ssl_sock_load_dh_params(SSL_CTX *ctx, const char *file)
{
	int ret = -1;
	BIO *in;
	DH *dh = NULL;
	/* If not present, use parameters generated using 'openssl dhparam 1024 -C':
	 * -----BEGIN DH PARAMETERS-----
	 * MIGHAoGBAJJAJDXDoS5E03MNjnjK36eOL1tRqVa/9NuOVlI+lpXmPjJQbP65EvKn
	 * fSLnG7VMhoCJO4KtG88zf393ltP7loGB2bofcDSr+x+XsxBM8yA/Zj6BmQt+CQ9s
	 * TF7hoOV+wXTT6ErZ5y5qx9pq6hLfKXwTGFT78hrE6HnCO7xgtPdTAgEC
	 * -----END DH PARAMETERS-----
	*/
	static const unsigned char dh1024_p[] = {
		0x92, 0x40, 0x24, 0x35, 0xC3, 0xA1, 0x2E, 0x44, 0xD3, 0x73, 0x0D, 0x8E,
		0x78, 0xCA, 0xDF, 0xA7, 0x8E, 0x2F, 0x5B, 0x51, 0xA9, 0x56, 0xBF, 0xF4,
		0xDB, 0x8E, 0x56, 0x52, 0x3E, 0x96, 0x95, 0xE6, 0x3E, 0x32, 0x50, 0x6C,
		0xFE, 0xB9, 0x12, 0xF2, 0xA7, 0x7D, 0x22, 0xE7, 0x1B, 0xB5, 0x4C, 0x86,
		0x80, 0x89, 0x3B, 0x82, 0xAD, 0x1B, 0xCF, 0x33, 0x7F, 0x7F, 0x77, 0x96,
		0xD3, 0xFB, 0x96, 0x81, 0x81, 0xD9, 0xBA, 0x1F, 0x70, 0x34, 0xAB, 0xFB,
		0x1F, 0x97, 0xB3, 0x10, 0x4C, 0xF3, 0x20, 0x3F, 0x66, 0x3E, 0x81, 0x99,
		0x0B, 0x7E, 0x09, 0x0F, 0x6C, 0x4C, 0x5E, 0xE1, 0xA0, 0xE5, 0x7E, 0xC1,
		0x74, 0xD3, 0xE8, 0x4A, 0xD9, 0xE7, 0x2E, 0x6A, 0xC7, 0xDA, 0x6A, 0xEA,
		0x12, 0xDF, 0x29, 0x7C, 0x13, 0x18, 0x54, 0xFB, 0xF2, 0x1A, 0xC4, 0xE8,
		0x79, 0xC2, 0x3B, 0xBC, 0x60, 0xB4, 0xF7, 0x53,
	};
	static const unsigned char dh1024_g[] = {
		0x02,
	};

	in = BIO_new(BIO_s_file());
	if (in == NULL)
		goto end;

	if (BIO_read_filename(in, file) <= 0)
		goto end;

	dh = PEM_read_bio_DHparams(in, NULL, ctx->default_passwd_callback, ctx->default_passwd_callback_userdata);
	if (!dh) {
		/* Clear openssl global errors stack */
		ERR_clear_error();

		dh = DH_new();
		if (dh == NULL)
			goto end;

		dh->p = BN_bin2bn(dh1024_p, sizeof(dh1024_p), NULL);
		if (dh->p == NULL)
			goto end;

		dh->g = BN_bin2bn(dh1024_g, sizeof(dh1024_g), NULL);
		if (dh->g == NULL)
			goto end;

		ret = 0; /* DH params not found */
	}
	else
		ret = 1;

	SSL_CTX_set_tmp_dh(ctx, dh);

end:
	if (dh)
		DH_free(dh);

	if (in)
		BIO_free(in);

	return ret;
}
#endif

static int ssl_sock_add_cert_sni(SSL_CTX *ctx, struct bind_conf *s, char *name, int order)
{
	struct sni_ctx *sc;
	int wild = 0, neg = 0;

	if (*name == '!') {
		neg = 1;
		name++;
	}
	if (*name == '*') {
		wild = 1;
		name++;
	}
	/* !* filter is a nop */
	if (neg && wild)
		return order;
	if (*name) {
		int j, len;
		len = strlen(name);
		sc = malloc(sizeof(struct sni_ctx) + len + 1);
		for (j = 0; j < len; j++)
			sc->name.key[j] = tolower(name[j]);
		sc->name.key[len] = 0;
		sc->ctx = ctx;
		sc->order = order++;
		sc->neg = neg;
		if (wild)
			ebst_insert(&s->sni_w_ctx, &sc->name);
		else
			ebst_insert(&s->sni_ctx, &sc->name);
	}
	return order;
}

/* Loads a certificate key and CA chain from a file. Returns 0 on error, -1 if
 * an early error happens and the caller must call SSL_CTX_free() by itelf.
 */
static int ssl_sock_load_cert_chain_file(SSL_CTX *ctx, const char *file, struct bind_conf *s, char **sni_filter, int fcount)
{
	BIO *in;
	X509 *x = NULL, *ca;
	int i, err;
	int ret = -1;
	int order = 0;
	X509_NAME *xname;
	char *str;
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
	STACK_OF(GENERAL_NAME) *names;
#endif

	in = BIO_new(BIO_s_file());
	if (in == NULL)
		goto end;

	if (BIO_read_filename(in, file) <= 0)
		goto end;

	x = PEM_read_bio_X509_AUX(in, NULL, ctx->default_passwd_callback, ctx->default_passwd_callback_userdata);
	if (x == NULL)
		goto end;

	if (fcount) {
		while (fcount--)
			order = ssl_sock_add_cert_sni(ctx, s, sni_filter[fcount], order);
	}
	else {
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
		names = X509_get_ext_d2i(x, NID_subject_alt_name, NULL, NULL);
		if (names) {
			for (i = 0; i < sk_GENERAL_NAME_num(names); i++) {
				GENERAL_NAME *name = sk_GENERAL_NAME_value(names, i);
				if (name->type == GEN_DNS) {
					if (ASN1_STRING_to_UTF8((unsigned char **)&str, name->d.dNSName) >= 0) {
						order = ssl_sock_add_cert_sni(ctx, s, str, order);
						OPENSSL_free(str);
					}
				}
			}
			sk_GENERAL_NAME_pop_free(names, GENERAL_NAME_free);
		}
#endif /* SSL_CTRL_SET_TLSEXT_HOSTNAME */
		xname = X509_get_subject_name(x);
		i = -1;
		while ((i = X509_NAME_get_index_by_NID(xname, NID_commonName, i)) != -1) {
			X509_NAME_ENTRY *entry = X509_NAME_get_entry(xname, i);
			if (ASN1_STRING_to_UTF8((unsigned char **)&str, entry->value) >= 0) {
				order = ssl_sock_add_cert_sni(ctx, s, str, order);
				OPENSSL_free(str);
			}
		}
	}

	ret = 0; /* the caller must not free the SSL_CTX argument anymore */
	if (!SSL_CTX_use_certificate(ctx, x))
		goto end;

	if (ctx->extra_certs != NULL) {
		sk_X509_pop_free(ctx->extra_certs, X509_free);
		ctx->extra_certs = NULL;
	}

	while ((ca = PEM_read_bio_X509(in, NULL, ctx->default_passwd_callback, ctx->default_passwd_callback_userdata))) {
		if (!SSL_CTX_add_extra_chain_cert(ctx, ca)) {
			X509_free(ca);
			goto end;
		}
	}

	err = ERR_get_error();
	if (!err || (ERR_GET_LIB(err) == ERR_LIB_PEM && ERR_GET_REASON(err) == PEM_R_NO_START_LINE)) {
		/* we successfully reached the last cert in the file */
		ret = 1;
	}
	ERR_clear_error();

end:
	if (x)
		X509_free(x);

	if (in)
		BIO_free(in);

	return ret;
}

static int ssl_sock_load_cert_file(const char *path, struct bind_conf *bind_conf, struct proxy *curproxy, char **sni_filter, int fcount, char **err)
{
	int ret;
	SSL_CTX *ctx;

	ctx = SSL_CTX_new(SSLv23_server_method());
	if (!ctx) {
		memprintf(err, "%sunable to allocate SSL context for cert '%s'.\n",
		          err && *err ? *err : "", path);
		return 1;
	}

	if (SSL_CTX_use_PrivateKey_file(ctx, path, SSL_FILETYPE_PEM) <= 0) {
		memprintf(err, "%sunable to load SSL private key from PEM file '%s'.\n",
		          err && *err ? *err : "", path);
		SSL_CTX_free(ctx);
		return 1;
	}

	ret = ssl_sock_load_cert_chain_file(ctx, path, bind_conf, sni_filter, fcount);
	if (ret <= 0) {
		memprintf(err, "%sunable to load SSL certificate from PEM file '%s'.\n",
		          err && *err ? *err : "", path);
		if (ret < 0) /* serious error, must do that ourselves */
			SSL_CTX_free(ctx);
		return 1;
	}

	if (SSL_CTX_check_private_key(ctx) <= 0) {
		memprintf(err, "%sinconsistencies between private key and certificate loaded from PEM file '%s'.\n",
		          err && *err ? *err : "", path);
		return 1;
	}

	/* we must not free the SSL_CTX anymore below, since it's already in
	 * the tree, so it will be discovered and cleaned in time.
	 */
#ifndef OPENSSL_NO_DH
	ret = ssl_sock_load_dh_params(ctx, path);
	if (ret < 0) {
		if (err)
			memprintf(err, "%sunable to load DH parameters from file '%s'.\n",
				  *err ? *err : "", path);
		return 1;
	}
#endif

#ifndef SSL_CTRL_SET_TLSEXT_HOSTNAME
	if (bind_conf->default_ctx) {
		memprintf(err, "%sthis version of openssl cannot load multiple SSL certificates.\n",
		          err && *err ? *err : "");
		return 1;
	}
#endif
	if (!bind_conf->default_ctx)
		bind_conf->default_ctx = ctx;

	return 0;
}

int ssl_sock_load_cert(char *path, struct bind_conf *bind_conf, struct proxy *curproxy, char **err)
{
	struct dirent *de;
	DIR *dir;
	struct stat buf;
	char *end;
	char fp[MAXPATHLEN+1];
	int cfgerr = 0;

	if (!(dir = opendir(path)))
		return ssl_sock_load_cert_file(path, bind_conf, curproxy, NULL, 0, err);

	/* strip trailing slashes, including first one */
	for (end = path + strlen(path) - 1; end >= path && *end == '/'; end--)
		*end = 0;

	while ((de = readdir(dir))) {
		snprintf(fp, sizeof(fp), "%s/%s", path, de->d_name);
		if (stat(fp, &buf) != 0) {
			memprintf(err, "%sunable to stat SSL certificate from file '%s' : %s.\n",
			          err && *err ? *err : "", fp, strerror(errno));
			cfgerr++;
			continue;
		}
		if (!S_ISREG(buf.st_mode))
			continue;
		cfgerr += ssl_sock_load_cert_file(fp, bind_conf, curproxy, NULL, 0, err);
	}
	closedir(dir);
	return cfgerr;
}

/* Make sure openssl opens /dev/urandom before the chroot. The work is only
 * done once. Zero is returned if the operation fails. No error is returned
 * if the random is said as not implemented, because we expect that openssl
 * will use another method once needed.
 */
static int ssl_initialize_random()
{
	unsigned char random;
	static int random_initialized = 0;

	if (!random_initialized && RAND_bytes(&random, 1) != 0)
		random_initialized = 1;

	return random_initialized;
}

int ssl_sock_load_cert_list_file(char *file, struct bind_conf *bind_conf, struct proxy *curproxy, char **err)
{
	char thisline[LINESIZE];
	FILE *f;
	int linenum = 0;
	int cfgerr = 0;

	if ((f = fopen(file, "r")) == NULL) {
		memprintf(err, "cannot open file '%s' : %s", file, strerror(errno));
		return 1;
	}

	while (fgets(thisline, sizeof(thisline), f) != NULL) {
		int arg;
		int newarg;
		char *end;
		char *args[MAX_LINE_ARGS + 1];
		char *line = thisline;

		linenum++;
		end = line + strlen(line);
		if (end-line == sizeof(thisline)-1 && *(end-1) != '\n') {
			/* Check if we reached the limit and the last char is not \n.
			 * Watch out for the last line without the terminating '\n'!
			 */
			memprintf(err, "line %d too long in file '%s', limit is %d characters",
				  linenum, file, (int)sizeof(thisline)-1);
			cfgerr = 1;
			break;
		}

		arg = 0;
		newarg = 1;
		while (*line) {
			if (*line == '#' || *line == '\n' || *line == '\r') {
				/* end of string, end of loop */
				*line = 0;
				break;
			}
			else if (isspace(*line)) {
				newarg = 1;
				*line = 0;
			}
			else if (newarg) {
				if (arg == MAX_LINE_ARGS) {
					memprintf(err, "too many args on line %d in file '%s'.",
						  linenum, file);
					cfgerr = 1;
					break;
				}
				newarg = 0;
				args[arg++] = line;
			}
			line++;
		}
		if (cfgerr)
			break;

		/* empty line */
		if (!arg)
			continue;

		cfgerr = ssl_sock_load_cert_file(args[0], bind_conf, curproxy, &args[1], arg-1, err);
		if (cfgerr) {
			memprintf(err, "error processing line %d in file '%s' : %s", linenum, file, *err);
			break;
		}
	}
	fclose(f);
	return cfgerr;
}

#ifndef SSL_OP_CIPHER_SERVER_PREFERENCE                 /* needs OpenSSL >= 0.9.7 */
#define SSL_OP_CIPHER_SERVER_PREFERENCE 0
#endif

#ifndef SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION   /* needs OpenSSL >= 0.9.7 */
#define SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION 0
#define SSL_renegotiate_pending(arg) 0
#endif
#ifndef SSL_OP_SINGLE_ECDH_USE                          /* needs OpenSSL >= 0.9.8 */
#define SSL_OP_SINGLE_ECDH_USE 0
#endif
#ifndef SSL_OP_NO_TICKET                                /* needs OpenSSL >= 0.9.8 */
#define SSL_OP_NO_TICKET 0
#endif
#ifndef SSL_OP_NO_COMPRESSION                           /* needs OpenSSL >= 0.9.9 */
#define SSL_OP_NO_COMPRESSION 0
#endif
#ifndef SSL_OP_NO_TLSv1_1                               /* needs OpenSSL >= 1.0.1 */
#define SSL_OP_NO_TLSv1_1 0
#endif
#ifndef SSL_OP_NO_TLSv1_2                               /* needs OpenSSL >= 1.0.1 */
#define SSL_OP_NO_TLSv1_2 0
#endif
#ifndef SSL_OP_SINGLE_DH_USE                            /* needs OpenSSL >= 0.9.6 */
#define SSL_OP_SINGLE_DH_USE 0
#endif
#ifndef SSL_OP_SINGLE_ECDH_USE                            /* needs OpenSSL >= 1.0.0 */
#define SSL_OP_SINGLE_ECDH_USE 0
#endif
#ifndef SSL_MODE_RELEASE_BUFFERS                        /* needs OpenSSL >= 1.0.0 */
#define SSL_MODE_RELEASE_BUFFERS 0
#endif
int ssl_sock_prepare_ctx(struct bind_conf *bind_conf, SSL_CTX *ctx, struct proxy *curproxy)
{
	int cfgerr = 0;
	int ssloptions =
		SSL_OP_ALL | /* all known workarounds for bugs */
		SSL_OP_NO_SSLv2 |
		SSL_OP_NO_COMPRESSION |
		SSL_OP_SINGLE_DH_USE |
		SSL_OP_SINGLE_ECDH_USE |
		SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION |
		SSL_OP_CIPHER_SERVER_PREFERENCE;
	int sslmode =
		SSL_MODE_ENABLE_PARTIAL_WRITE |
		SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
		SSL_MODE_RELEASE_BUFFERS;

	/* Make sure openssl opens /dev/urandom before the chroot */
	if (!ssl_initialize_random()) {
		Alert("OpenSSL random data generator initialization failed.\n");
		cfgerr++;
	}

	if (bind_conf->ssl_options & BC_SSL_O_NO_SSLV3)
		ssloptions |= SSL_OP_NO_SSLv3;
	if (bind_conf->ssl_options & BC_SSL_O_NO_TLSV10)
		ssloptions |= SSL_OP_NO_TLSv1;
	if (bind_conf->ssl_options & BC_SSL_O_NO_TLSV11)
		ssloptions |= SSL_OP_NO_TLSv1_1;
	if (bind_conf->ssl_options & BC_SSL_O_NO_TLSV12)
		ssloptions |= SSL_OP_NO_TLSv1_2;
	if (bind_conf->ssl_options & BC_SSL_O_NO_TLS_TICKETS)
		ssloptions |= SSL_OP_NO_TICKET;
	if (bind_conf->ssl_options & BC_SSL_O_USE_SSLV3)
		SSL_CTX_set_ssl_version(ctx, SSLv3_server_method());
	if (bind_conf->ssl_options & BC_SSL_O_USE_TLSV10)
		SSL_CTX_set_ssl_version(ctx, TLSv1_server_method());
#if SSL_OP_NO_TLSv1_1
	if (bind_conf->ssl_options & BC_SSL_O_USE_TLSV11)
		SSL_CTX_set_ssl_version(ctx, TLSv1_1_server_method());
#endif
#if SSL_OP_NO_TLSv1_2
	if (bind_conf->ssl_options & BC_SSL_O_USE_TLSV12)
		SSL_CTX_set_ssl_version(ctx, TLSv1_2_server_method());
#endif

	SSL_CTX_set_options(ctx, ssloptions);
	SSL_CTX_set_mode(ctx, sslmode);
	SSL_CTX_set_verify(ctx, bind_conf->verify ? bind_conf->verify : SSL_VERIFY_NONE, ssl_sock_bind_verifycbk);
	if (bind_conf->verify & SSL_VERIFY_PEER) {
		if (bind_conf->ca_file) {
			/* load CAfile to verify */
			if (!SSL_CTX_load_verify_locations(ctx, bind_conf->ca_file, NULL)) {
				Alert("Proxy '%s': unable to load CA file '%s' for bind '%s' at [%s:%d].\n",
				      curproxy->id, bind_conf->ca_file, bind_conf->arg, bind_conf->file, bind_conf->line);
				cfgerr++;
			}
			/* set CA names fo client cert request, function returns void */
			SSL_CTX_set_client_CA_list(ctx, SSL_load_client_CA_file(bind_conf->ca_file));
		}
#ifdef X509_V_FLAG_CRL_CHECK
		if (bind_conf->crl_file) {
			X509_STORE *store = SSL_CTX_get_cert_store(ctx);

			if (!store || !X509_STORE_load_locations(store, bind_conf->crl_file, NULL)) {
				Alert("Proxy '%s': unable to configure CRL file '%s' for bind '%s' at [%s:%d].\n",
				      curproxy->id, bind_conf->ca_file, bind_conf->arg, bind_conf->file, bind_conf->line);
				cfgerr++;
			}
			else {
				X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK|X509_V_FLAG_CRL_CHECK_ALL);
			}
		}
#endif
		ERR_clear_error();
	}

	if (global.tune.ssllifetime)
		SSL_CTX_set_timeout(ctx, global.tune.ssllifetime);

	shared_context_set_cache(ctx);
	if (bind_conf->ciphers &&
	    !SSL_CTX_set_cipher_list(ctx, bind_conf->ciphers)) {
		Alert("Proxy '%s': unable to set SSL cipher list to '%s' for bind '%s' at [%s:%d].\n",
		curproxy->id, bind_conf->ciphers, bind_conf->arg, bind_conf->file, bind_conf->line);
		cfgerr++;
	}

	SSL_CTX_set_info_callback(ctx, ssl_sock_infocbk);
#ifdef OPENSSL_NPN_NEGOTIATED
	if (bind_conf->npn_str)
		SSL_CTX_set_next_protos_advertised_cb(ctx, ssl_sock_advertise_npn_protos, bind_conf);
#endif
#ifdef OPENSSL_ALPN_NEGOTIATED
	if (bind_conf->alpn_str)
		SSL_CTX_set_alpn_advertised_cb(ctx, ssl_sock_advertise_alpn_protos, bind_conf);
#endif

#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
	SSL_CTX_set_tlsext_servername_callback(ctx, ssl_sock_switchctx_cbk);
	SSL_CTX_set_tlsext_servername_arg(ctx, bind_conf);
#endif
#if defined(SSL_CTX_set_tmp_ecdh) && !defined(OPENSSL_NO_ECDH)
	{
		int i;
		EC_KEY  *ecdh;

		i = OBJ_sn2nid(bind_conf->ecdhe ? bind_conf->ecdhe : ECDHE_DEFAULT_CURVE);
		if (!i || ((ecdh = EC_KEY_new_by_curve_name(i)) == NULL)) {
			Alert("Proxy '%s': unable to set elliptic named curve to '%s' for bind '%s' at [%s:%d].\n",
			      curproxy->id, bind_conf->ecdhe ? bind_conf->ecdhe : ECDHE_DEFAULT_CURVE,
			      bind_conf->arg, bind_conf->file, bind_conf->line);
			cfgerr++;
		}
		else {
			SSL_CTX_set_tmp_ecdh(ctx, ecdh);
			EC_KEY_free(ecdh);
		}
	}
#endif

	return cfgerr;
}

static int ssl_sock_srv_hostcheck(const char *pattern, const char *hostname)
{
	const char *pattern_wildcard, *pattern_left_label_end, *hostname_left_label_end;
	size_t prefixlen, suffixlen;

	/* Trivial case */
	if (strcmp(pattern, hostname) == 0)
		return 1;

	/* The rest of this logic is based on RFC 6125, section 6.4.3
	 * (http://tools.ietf.org/html/rfc6125#section-6.4.3) */

	pattern_wildcard = NULL;
	pattern_left_label_end = pattern;
	while (*pattern_left_label_end != '.') {
		switch (*pattern_left_label_end) {
			case 0:
				/* End of label not found */
				return 0;
			case '*':
				/* If there is more than one wildcards */
                                if (pattern_wildcard)
                                        return 0;
				pattern_wildcard = pattern_left_label_end;
				break;
		}
		pattern_left_label_end++;
	}

	/* If it's not trivial and there is no wildcard, it can't
	 * match */
	if (!pattern_wildcard)
		return 0;

	/* Make sure all labels match except the leftmost */
	hostname_left_label_end = strchr(hostname, '.');
	if (!hostname_left_label_end
	    || strcmp(pattern_left_label_end, hostname_left_label_end) != 0)
		return 0;

	/* Make sure the leftmost label of the hostname is long enough
	 * that the wildcard can match */
	if (hostname_left_label_end - hostname < (pattern_left_label_end - pattern) - 1)
		return 0;

	/* Finally compare the string on either side of the
	 * wildcard */
	prefixlen = pattern_wildcard - pattern;
	suffixlen = pattern_left_label_end - (pattern_wildcard + 1);
	if ((prefixlen && (memcmp(pattern, hostname, prefixlen) != 0))
	    || (suffixlen && (memcmp(pattern_wildcard + 1, hostname_left_label_end - suffixlen, suffixlen) != 0)))
		return 0;

	return 1;
}

static int ssl_sock_srv_verifycbk(int ok, X509_STORE_CTX *ctx)
{
	SSL *ssl;
	struct connection *conn;
	char *servername;

	int depth;
	X509 *cert;
	STACK_OF(GENERAL_NAME) *alt_names;
	int i;
	X509_NAME *cert_subject;
	char *str;

	if (ok == 0)
		return ok;

	ssl = X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
	conn = (struct connection *)SSL_get_app_data(ssl);

	servername = objt_server(conn->target)->ssl_ctx.verify_host;

	/* We only need to verify the CN on the actual server cert,
	 * not the indirect CAs */
	depth = X509_STORE_CTX_get_error_depth(ctx);
	if (depth != 0)
		return ok;

	/* At this point, the cert is *not* OK unless we can find a
	 * hostname match */
	ok = 0;

	cert = X509_STORE_CTX_get_current_cert(ctx);
	/* It seems like this might happen if verify peer isn't set */
	if (!cert)
		return ok;

	alt_names = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
	if (alt_names) {
		for (i = 0; !ok && i < sk_GENERAL_NAME_num(alt_names); i++) {
			GENERAL_NAME *name = sk_GENERAL_NAME_value(alt_names, i);
			if (name->type == GEN_DNS) {
#if OPENSSL_VERSION_NUMBER < 0x00907000L
				if (ASN1_STRING_to_UTF8((unsigned char **)&str, name->d.ia5) >= 0) {
#else
				if (ASN1_STRING_to_UTF8((unsigned char **)&str, name->d.dNSName) >= 0) {
#endif
					ok = ssl_sock_srv_hostcheck(str, servername);
					OPENSSL_free(str);
				}
			}
		}
		sk_GENERAL_NAME_pop_free(alt_names, GENERAL_NAME_free);
	}

	cert_subject = X509_get_subject_name(cert);
	i = -1;
	while (!ok && (i = X509_NAME_get_index_by_NID(cert_subject, NID_commonName, i)) != -1) {
		X509_NAME_ENTRY *entry = X509_NAME_get_entry(cert_subject, i);
		if (ASN1_STRING_to_UTF8((unsigned char **)&str, entry->value) >= 0) {
			ok = ssl_sock_srv_hostcheck(str, servername);
			OPENSSL_free(str);
		}
	}

	return ok;
}

/* prepare ssl context from servers options. Returns an error count */
int ssl_sock_prepare_srv_ctx(struct server *srv, struct proxy *curproxy)
{
	int cfgerr = 0;
	int options =
		SSL_OP_ALL | /* all known workarounds for bugs */
		SSL_OP_NO_SSLv2 |
		SSL_OP_NO_COMPRESSION;
	int mode =
		SSL_MODE_ENABLE_PARTIAL_WRITE |
		SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
		SSL_MODE_RELEASE_BUFFERS;

	/* Make sure openssl opens /dev/urandom before the chroot */
	if (!ssl_initialize_random()) {
		Alert("OpenSSL random data generator initialization failed.\n");
		cfgerr++;
	}

	 /* Initiate SSL context for current server */
	srv->ssl_ctx.reused_sess = NULL;
	if (srv->use_ssl)
		srv->xprt = &ssl_sock;
	if (srv->check.use_ssl)
		srv->check_common.xprt = &ssl_sock;

	srv->ssl_ctx.ctx = SSL_CTX_new(SSLv23_client_method());
	if (!srv->ssl_ctx.ctx) {
		Alert("config : %s '%s', server '%s': unable to allocate ssl context.\n",
		      proxy_type_str(curproxy), curproxy->id,
		      srv->id);
		cfgerr++;
		return cfgerr;
	}
	if (srv->ssl_ctx.client_crt) {
		if (SSL_CTX_use_PrivateKey_file(srv->ssl_ctx.ctx, srv->ssl_ctx.client_crt, SSL_FILETYPE_PEM) <= 0) {
			Alert("config : %s '%s', server '%s': unable to load SSL private key from PEM file '%s'.\n",
			      proxy_type_str(curproxy), curproxy->id,
			      srv->id, srv->ssl_ctx.client_crt);
			cfgerr++;
		}
		else if (SSL_CTX_use_certificate_chain_file(srv->ssl_ctx.ctx, srv->ssl_ctx.client_crt) <= 0) {
			Alert("config : %s '%s', server '%s': unable to load ssl certificate from PEM file '%s'.\n",
			      proxy_type_str(curproxy), curproxy->id,
			      srv->id, srv->ssl_ctx.client_crt);
			cfgerr++;
		}
		else if (SSL_CTX_check_private_key(srv->ssl_ctx.ctx) <= 0) {
			Alert("config : %s '%s', server '%s': inconsistencies between private key and certificate loaded from PEM file '%s'.\n",
			      proxy_type_str(curproxy), curproxy->id,
			      srv->id, srv->ssl_ctx.client_crt);
			cfgerr++;
		}
	}

	if (srv->ssl_ctx.options & SRV_SSL_O_NO_SSLV3)
		options |= SSL_OP_NO_SSLv3;
	if (srv->ssl_ctx.options & SRV_SSL_O_NO_TLSV10)
		options |= SSL_OP_NO_TLSv1;
	if (srv->ssl_ctx.options & SRV_SSL_O_NO_TLSV11)
		options |= SSL_OP_NO_TLSv1_1;
	if (srv->ssl_ctx.options & SRV_SSL_O_NO_TLSV12)
		options |= SSL_OP_NO_TLSv1_2;
	if (srv->ssl_ctx.options & SRV_SSL_O_NO_TLS_TICKETS)
		options |= SSL_OP_NO_TICKET;
	if (srv->ssl_ctx.options & SRV_SSL_O_USE_SSLV3)
		SSL_CTX_set_ssl_version(srv->ssl_ctx.ctx, SSLv3_client_method());
	if (srv->ssl_ctx.options & SRV_SSL_O_USE_TLSV10)
		SSL_CTX_set_ssl_version(srv->ssl_ctx.ctx, TLSv1_client_method());
#if SSL_OP_NO_TLSv1_1
	if (srv->ssl_ctx.options & SRV_SSL_O_USE_TLSV11)
		SSL_CTX_set_ssl_version(srv->ssl_ctx.ctx, TLSv1_1_client_method());
#endif
#if SSL_OP_NO_TLSv1_2
	if (srv->ssl_ctx.options & SRV_SSL_O_USE_TLSV12)
		SSL_CTX_set_ssl_version(srv->ssl_ctx.ctx, TLSv1_2_client_method());
#endif

	SSL_CTX_set_options(srv->ssl_ctx.ctx, options);
	SSL_CTX_set_mode(srv->ssl_ctx.ctx, mode);
	SSL_CTX_set_verify(srv->ssl_ctx.ctx,
	                   srv->ssl_ctx.verify ? srv->ssl_ctx.verify : SSL_VERIFY_NONE,
	                   srv->ssl_ctx.verify_host ? ssl_sock_srv_verifycbk : NULL);
	if (srv->ssl_ctx.verify & SSL_VERIFY_PEER) {
		if (srv->ssl_ctx.ca_file) {
			/* load CAfile to verify */
			if (!SSL_CTX_load_verify_locations(srv->ssl_ctx.ctx, srv->ssl_ctx.ca_file, NULL)) {
				Alert("Proxy '%s', server '%s' |%s:%d] unable to load CA file '%s'.\n",
				      curproxy->id, srv->id,
				      srv->conf.file, srv->conf.line, srv->ssl_ctx.ca_file);
				cfgerr++;
			}
		}
#ifdef X509_V_FLAG_CRL_CHECK
		if (srv->ssl_ctx.crl_file) {
			X509_STORE *store = SSL_CTX_get_cert_store(srv->ssl_ctx.ctx);

			if (!store || !X509_STORE_load_locations(store, srv->ssl_ctx.crl_file, NULL)) {
				Alert("Proxy '%s', server '%s' |%s:%d] unable to configure CRL file '%s'.\n",
				      curproxy->id, srv->id,
				      srv->conf.file, srv->conf.line, srv->ssl_ctx.crl_file);
				cfgerr++;
			}
			else {
				X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK|X509_V_FLAG_CRL_CHECK_ALL);
			}
		}
#endif
	}

	if (global.tune.ssllifetime)
		SSL_CTX_set_timeout(srv->ssl_ctx.ctx, global.tune.ssllifetime);

	SSL_CTX_set_session_cache_mode(srv->ssl_ctx.ctx, SSL_SESS_CACHE_OFF);
	if (srv->ssl_ctx.ciphers &&
		!SSL_CTX_set_cipher_list(srv->ssl_ctx.ctx, srv->ssl_ctx.ciphers)) {
		Alert("Proxy '%s', server '%s' [%s:%d] : unable to set SSL cipher list to '%s'.\n",
		      curproxy->id, srv->id,
		      srv->conf.file, srv->conf.line, srv->ssl_ctx.ciphers);
		cfgerr++;
	}

	return cfgerr;
}

/* Walks down the two trees in bind_conf and prepares all certs. The pointer may
 * be NULL, in which case nothing is done. Returns the number of errors
 * encountered.
 */
int ssl_sock_prepare_all_ctx(struct bind_conf *bind_conf, struct proxy *px)
{
	struct ebmb_node *node;
	struct sni_ctx *sni;
	int err = 0;

	if (!bind_conf || !bind_conf->is_ssl)
		return 0;

	node = ebmb_first(&bind_conf->sni_ctx);
	while (node) {
		sni = ebmb_entry(node, struct sni_ctx, name);
		if (!sni->order) /* only initialize the CTX on its first occurrence */
			err += ssl_sock_prepare_ctx(bind_conf, sni->ctx, px);
		node = ebmb_next(node);
	}

	node = ebmb_first(&bind_conf->sni_w_ctx);
	while (node) {
		sni = ebmb_entry(node, struct sni_ctx, name);
		if (!sni->order) /* only initialize the CTX on its first occurrence */
			err += ssl_sock_prepare_ctx(bind_conf, sni->ctx, px);
		node = ebmb_next(node);
	}
	return err;
}

/* Walks down the two trees in bind_conf and frees all the certs. The pointer may
 * be NULL, in which case nothing is done. The default_ctx is nullified too.
 */
void ssl_sock_free_all_ctx(struct bind_conf *bind_conf)
{
	struct ebmb_node *node, *back;
	struct sni_ctx *sni;

	if (!bind_conf || !bind_conf->is_ssl)
		return;

	node = ebmb_first(&bind_conf->sni_ctx);
	while (node) {
		sni = ebmb_entry(node, struct sni_ctx, name);
		back = ebmb_next(node);
		ebmb_delete(node);
		if (!sni->order) /* only free the CTX on its first occurrence */
			SSL_CTX_free(sni->ctx);
		free(sni);
		node = back;
	}

	node = ebmb_first(&bind_conf->sni_w_ctx);
	while (node) {
		sni = ebmb_entry(node, struct sni_ctx, name);
		back = ebmb_next(node);
		ebmb_delete(node);
		if (!sni->order) /* only free the CTX on its first occurrence */
			SSL_CTX_free(sni->ctx);
		free(sni);
		node = back;
	}

	bind_conf->default_ctx = NULL;
}

/*
 * This function is called if SSL * context is not yet allocated. The function
 * is designed to be called before any other data-layer operation and sets the
 * handshake flag on the connection. It is safe to call it multiple times.
 * It returns 0 on success and -1 in error case.
 */
static int ssl_sock_init(struct connection *conn)
{
	/* already initialized */
	if (conn->xprt_ctx)
		return 0;

	if (!(conn->flags & CO_FL_CTRL_READY))
		return 0;

	if (global.maxsslconn && sslconns >= global.maxsslconn) {
		conn->err_code = CO_ER_SSL_TOO_MANY;
		return -1;
	}

	/* If it is in client mode initiate SSL session
	   in connect state otherwise accept state */
	if (objt_server(conn->target)) {
		/* Alloc a new SSL session ctx */
		conn->xprt_ctx = SSL_new(objt_server(conn->target)->ssl_ctx.ctx);
		if (!conn->xprt_ctx) {
			conn->err_code = CO_ER_SSL_NO_MEM;
			return -1;
		}

		SSL_set_connect_state(conn->xprt_ctx);
		if (objt_server(conn->target)->ssl_ctx.reused_sess)
			SSL_set_session(conn->xprt_ctx, objt_server(conn->target)->ssl_ctx.reused_sess);

		/* set fd on SSL session context */
		SSL_set_fd(conn->xprt_ctx, conn->t.sock.fd);

		/* set connection pointer */
		SSL_set_app_data(conn->xprt_ctx, conn);

		/* leave init state and start handshake */
		conn->flags |= CO_FL_SSL_WAIT_HS | CO_FL_WAIT_L6_CONN;

		sslconns++;
		return 0;
	}
	else if (objt_listener(conn->target)) {
		/* Alloc a new SSL session ctx */
		conn->xprt_ctx = SSL_new(objt_listener(conn->target)->bind_conf->default_ctx);
		if (!conn->xprt_ctx) {
			conn->err_code = CO_ER_SSL_NO_MEM;
			return -1;
		}

		SSL_set_accept_state(conn->xprt_ctx);

		/* set fd on SSL session context */
		SSL_set_fd(conn->xprt_ctx, conn->t.sock.fd);

		/* set connection pointer */
		SSL_set_app_data(conn->xprt_ctx, conn);

		/* leave init state and start handshake */
		conn->flags |= CO_FL_SSL_WAIT_HS | CO_FL_WAIT_L6_CONN;

		sslconns++;
		return 0;
	}
	/* don't know how to handle such a target */
	conn->err_code = CO_ER_SSL_NO_TARGET;
	return -1;
}


/* This is the callback which is used when an SSL handshake is pending. It
 * updates the FD status if it wants some polling before being called again.
 * It returns 0 if it fails in a fatal way or needs to poll to go further,
 * otherwise it returns non-zero and removes itself from the connection's
 * flags (the bit is provided in <flag> by the caller).
 */
int ssl_sock_handshake(struct connection *conn, unsigned int flag)
{
	int ret;

	if (!(conn->flags & CO_FL_CTRL_READY))
		return 0;

	if (!conn->xprt_ctx)
		goto out_error;

	/* If we use SSL_do_handshake to process a reneg initiated by
	 * the remote peer, it sometimes returns SSL_ERROR_SSL.
	 * Usually SSL_write and SSL_read are used and process implicitly
	 * the reneg handshake.
	 * Here we use SSL_peek as a workaround for reneg.
	 */
	if ((conn->flags & CO_FL_CONNECTED) && SSL_renegotiate_pending(conn->xprt_ctx)) {
		char c;

		ret = SSL_peek(conn->xprt_ctx, &c, 1);
		if (ret <= 0) {
			/* handshake may have not been completed, let's find why */
			ret = SSL_get_error(conn->xprt_ctx, ret);
			if (ret == SSL_ERROR_WANT_WRITE) {
				/* SSL handshake needs to write, L4 connection may not be ready */
				__conn_sock_stop_recv(conn);
				__conn_sock_poll_send(conn);
				return 0;
			}
			else if (ret == SSL_ERROR_WANT_READ) {
				/* handshake may have been completed but we have
				 * no more data to read.
                                 */
				if (!SSL_renegotiate_pending(conn->xprt_ctx)) {
					ret = 1;
					goto reneg_ok;
				}
				/* SSL handshake needs to read, L4 connection is ready */
				if (conn->flags & CO_FL_WAIT_L4_CONN)
					conn->flags &= ~CO_FL_WAIT_L4_CONN;
				__conn_sock_stop_send(conn);
				__conn_sock_poll_recv(conn);
				return 0;
			}
			else if (ret == SSL_ERROR_SYSCALL) {
				/* if errno is null, then connection was successfully established */
				if (!errno && conn->flags & CO_FL_WAIT_L4_CONN)
					conn->flags &= ~CO_FL_WAIT_L4_CONN;
				if (!conn->err_code) {
					if (!((SSL *)conn->xprt_ctx)->packet_length)
						if (!errno)
							conn->err_code = CO_ER_SSL_EMPTY;
						else
							conn->err_code = CO_ER_SSL_ABORT;
					else
						conn->err_code = CO_ER_SSL_HANDSHAKE;
				}
				goto out_error;
			}
			else {
				/* Fail on all other handshake errors */
				/* Note: OpenSSL may leave unread bytes in the socket's
				 * buffer, causing an RST to be emitted upon close() on
				 * TCP sockets. We first try to drain possibly pending
				 * data to avoid this as much as possible.
				 */
				if ((conn->flags & CO_FL_CTRL_READY) && conn->ctrl && conn->ctrl->drain)
					conn->ctrl->drain(conn->t.sock.fd);
				if (!conn->err_code)
					conn->err_code = CO_ER_SSL_HANDSHAKE;
				goto out_error;
			}
		}
		/* read some data: consider handshake completed */
		goto reneg_ok;
	}

	ret = SSL_do_handshake(conn->xprt_ctx);
	if (ret != 1) {
		/* handshake did not complete, let's find why */
		ret = SSL_get_error(conn->xprt_ctx, ret);

		if (ret == SSL_ERROR_WANT_WRITE) {
			/* SSL handshake needs to write, L4 connection may not be ready */
			__conn_sock_stop_recv(conn);
			__conn_sock_poll_send(conn);
			return 0;
		}
		else if (ret == SSL_ERROR_WANT_READ) {
			/* SSL handshake needs to read, L4 connection is ready */
			if (conn->flags & CO_FL_WAIT_L4_CONN)
				conn->flags &= ~CO_FL_WAIT_L4_CONN;
			__conn_sock_stop_send(conn);
			__conn_sock_poll_recv(conn);
			return 0;
		}
		else if (ret == SSL_ERROR_SYSCALL) {
			/* if errno is null, then connection was successfully established */
			if (!errno && conn->flags & CO_FL_WAIT_L4_CONN)
				conn->flags &= ~CO_FL_WAIT_L4_CONN;

			if (!((SSL *)conn->xprt_ctx)->packet_length)
				if (!errno)
					conn->err_code = CO_ER_SSL_EMPTY;
				else
					conn->err_code = CO_ER_SSL_ABORT;
			else
				conn->err_code = CO_ER_SSL_HANDSHAKE;
			goto out_error;
		}
		else {
			/* Fail on all other handshake errors */
			/* Note: OpenSSL may leave unread bytes in the socket's
			 * buffer, causing an RST to be emitted upon close() on
			 * TCP sockets. We first try to drain possibly pending
			 * data to avoid this as much as possible.
			 */
			if ((conn->flags & CO_FL_CTRL_READY) && conn->ctrl && conn->ctrl->drain)
				conn->ctrl->drain(conn->t.sock.fd);
			if (!conn->err_code)
				conn->err_code = CO_ER_SSL_HANDSHAKE;
			goto out_error;
		}
	}

reneg_ok:

	/* Handshake succeeded */
	if (objt_server(conn->target)) {
		if (!SSL_session_reused(conn->xprt_ctx)) {
			/* check if session was reused, if not store current session on server for reuse */
			if (objt_server(conn->target)->ssl_ctx.reused_sess)
				SSL_SESSION_free(objt_server(conn->target)->ssl_ctx.reused_sess);

			objt_server(conn->target)->ssl_ctx.reused_sess = SSL_get1_session(conn->xprt_ctx);
		}
	}

	/* The connection is now established at both layers, it's time to leave */
	conn->flags &= ~(flag | CO_FL_WAIT_L4_CONN | CO_FL_WAIT_L6_CONN);
	return 1;

 out_error:
	/* Clear openssl global errors stack */
	ERR_clear_error();

	/* free resumed session if exists */
	if (objt_server(conn->target) && objt_server(conn->target)->ssl_ctx.reused_sess) {
		SSL_SESSION_free(objt_server(conn->target)->ssl_ctx.reused_sess);
		objt_server(conn->target)->ssl_ctx.reused_sess = NULL;
	}

	/* Fail on all other handshake errors */
	conn->flags |= CO_FL_ERROR;
	if (!conn->err_code)
		conn->err_code = CO_ER_SSL_HANDSHAKE;
	return 0;
}

/* Receive up to <count> bytes from connection <conn>'s socket and store them
 * into buffer <buf>. The caller must ensure that <count> is always smaller
 * than the buffer's size. Only one call to recv() is performed, unless the
 * buffer wraps, in which case a second call may be performed. The connection's
 * flags are updated with whatever special event is detected (error, read0,
 * empty). The caller is responsible for taking care of those events and
 * avoiding the call if inappropriate. The function does not call the
 * connection's polling update function, so the caller is responsible for this.
 */
static int ssl_sock_to_buf(struct connection *conn, struct buffer *buf, int count)
{
	int ret, done = 0;
	int try = count;

	if (!conn->xprt_ctx)
		goto out_error;

	if (conn->flags & CO_FL_HANDSHAKE)
		/* a handshake was requested */
		return 0;

	/* compute the maximum block size we can read at once. */
	if (buffer_empty(buf)) {
		/* let's realign the buffer to optimize I/O */
		buf->p = buf->data;
	}
	else if (buf->data + buf->o < buf->p &&
		 buf->p + buf->i < buf->data + buf->size) {
		/* remaining space wraps at the end, with a moving limit */
		if (try > buf->data + buf->size - (buf->p + buf->i))
			try = buf->data + buf->size - (buf->p + buf->i);
	}

	/* read the largest possible block. For this, we perform only one call
	 * to recv() unless the buffer wraps and we exactly fill the first hunk,
	 * in which case we accept to do it once again. A new attempt is made on
	 * EINTR too.
	 */
	while (try) {
		ret = SSL_read(conn->xprt_ctx, bi_end(buf), try);
		if (conn->flags & CO_FL_ERROR) {
			/* CO_FL_ERROR may be set by ssl_sock_infocbk */
			goto out_error;
		}
		if (ret > 0) {
			buf->i += ret;
			done += ret;
			if (ret < try)
				break;
			count -= ret;
			try = count;
		}
		else if (ret == 0) {
			ret =  SSL_get_error(conn->xprt_ctx, ret);
			if (ret != SSL_ERROR_ZERO_RETURN) {
				/* error on protocol or underlying transport */
				if ((ret != SSL_ERROR_SYSCALL)
				     || (errno && (errno != EAGAIN)))
					conn->flags |= CO_FL_ERROR;

				/* Clear openssl global errors stack */
				ERR_clear_error();
			}
			goto read0;
		}
		else {
			ret =  SSL_get_error(conn->xprt_ctx, ret);
			if (ret == SSL_ERROR_WANT_WRITE) {
				/* handshake is running, and it needs to enable write */
				conn->flags |= CO_FL_SSL_WAIT_HS;
				__conn_sock_want_send(conn);
				break;
			}
			else if (ret == SSL_ERROR_WANT_READ) {
				if (SSL_renegotiate_pending(conn->xprt_ctx)) {
					/* handshake is running, and it may need to re-enable read */
					conn->flags |= CO_FL_SSL_WAIT_HS;
					__conn_sock_want_recv(conn);
					break;
				}
				/* we need to poll for retry a read later */
				__conn_data_poll_recv(conn);
				break;
			}
			/* otherwise it's a real error */
			goto out_error;
		}
	}
	return done;

 read0:
	conn_sock_read0(conn);
	return done;
 out_error:
	/* Clear openssl global errors stack */
	ERR_clear_error();

	conn->flags |= CO_FL_ERROR;
	return done;
}


/* Send all pending bytes from buffer <buf> to connection <conn>'s socket.
 * <flags> may contain MSG_MORE to make the system hold on without sending
 * data too fast, but this flag is ignored at the moment.
 * Only one call to send() is performed, unless the buffer wraps, in which case
 * a second call may be performed. The connection's flags are updated with
 * whatever special event is detected (error, empty). The caller is responsible
 * for taking care of those events and avoiding the call if inappropriate. The
 * function does not call the connection's polling update function, so the caller
 * is responsible for this.
 */
static int ssl_sock_from_buf(struct connection *conn, struct buffer *buf, int flags)
{
	int ret, try, done;

	done = 0;

	if (!conn->xprt_ctx)
		goto out_error;

	if (conn->flags & CO_FL_HANDSHAKE)
		/* a handshake was requested */
		return 0;

	/* send the largest possible block. For this we perform only one call
	 * to send() unless the buffer wraps and we exactly fill the first hunk,
	 * in which case we accept to do it once again.
	 */
	while (buf->o) {
		try = bo_contig_data(buf);

		if (global.tune.ssl_max_record && try > global.tune.ssl_max_record)
			try = global.tune.ssl_max_record;

		ret = SSL_write(conn->xprt_ctx, bo_ptr(buf), try);
		if (conn->flags & CO_FL_ERROR) {
			/* CO_FL_ERROR may be set by ssl_sock_infocbk */
			goto out_error;
		}
		if (ret > 0) {
			buf->o -= ret;
			done += ret;

			if (likely(buffer_empty(buf)))
				/* optimize data alignment in the buffer */
				buf->p = buf->data;

			/* if the system buffer is full, don't insist */
			if (ret < try)
				break;
		}
		else {
			ret = SSL_get_error(conn->xprt_ctx, ret);
			if (ret == SSL_ERROR_WANT_WRITE) {
				if (SSL_renegotiate_pending(conn->xprt_ctx)) {
					/* handshake is running, and it may need to re-enable write */
					conn->flags |= CO_FL_SSL_WAIT_HS;
					__conn_sock_want_send(conn);
					break;
				}
				/* we need to poll to retry a write later */
				__conn_data_poll_send(conn);
				break;
			}
			else if (ret == SSL_ERROR_WANT_READ) {
				/* handshake is running, and it needs to enable read */
				conn->flags |= CO_FL_SSL_WAIT_HS;
				__conn_sock_want_recv(conn);
				break;
			}
			goto out_error;
		}
	}
	return done;

 out_error:
	/* Clear openssl global errors stack */
	ERR_clear_error();

	conn->flags |= CO_FL_ERROR;
	return done;
}

static void ssl_sock_close(struct connection *conn) {

	if (conn->xprt_ctx) {
		SSL_free(conn->xprt_ctx);
		conn->xprt_ctx = NULL;
		sslconns--;
	}
}

/* This function tries to perform a clean shutdown on an SSL connection, and in
 * any case, flags the connection as reusable if no handshake was in progress.
 */
static void ssl_sock_shutw(struct connection *conn, int clean)
{
	if (conn->flags & CO_FL_HANDSHAKE)
		return;
	/* no handshake was in progress, try a clean ssl shutdown */
	if (clean && (SSL_shutdown(conn->xprt_ctx) <= 0)) {
		/* Clear openssl global errors stack */
		ERR_clear_error();
	}

	/* force flag on ssl to keep session in cache regardless shutdown result */
	SSL_set_shutdown(conn->xprt_ctx, SSL_SENT_SHUTDOWN);
}

/* used for logging, may be changed for a sample fetch later */
const char *ssl_sock_get_cipher_name(struct connection *conn)
{
	if (!conn->xprt && !conn->xprt_ctx)
		return NULL;
	return SSL_get_cipher_name(conn->xprt_ctx);
}

/* used for logging, may be changed for a sample fetch later */
const char *ssl_sock_get_proto_version(struct connection *conn)
{
	if (!conn->xprt && !conn->xprt_ctx)
		return NULL;
	return SSL_get_version(conn->xprt_ctx);
}

/* Extract a serial from a cert, and copy it to a chunk.
 * Returns 1 if serial is found and copied, 0 if no serial found and
 * -1 if output is not large enough.
 */
static int
ssl_sock_get_serial(X509 *crt, struct chunk *out)
{
	ASN1_INTEGER *serial;

	serial = X509_get_serialNumber(crt);
	if (!serial)
		return 0;

	if (out->size < serial->length)
		return -1;

	memcpy(out->str, serial->data, serial->length);
	out->len = serial->length;
	return 1;
}


/* Copy Date in ASN1_UTCTIME format in struct chunk out.
 * Returns 1 if serial is found and copied, 0 if no valid time found
 * and -1 if output is not large enough.
 */
static int
ssl_sock_get_time(ASN1_TIME *tm, struct chunk *out)
{
	if (tm->type == V_ASN1_GENERALIZEDTIME) {
		ASN1_GENERALIZEDTIME *gentm = (ASN1_GENERALIZEDTIME *)tm;

		if (gentm->length < 12)
			return 0;
		if (gentm->data[0] != 0x32 || gentm->data[1] != 0x30)
			return 0;
		if (out->size < gentm->length-2)
			return -1;

		memcpy(out->str, gentm->data+2, gentm->length-2);
		out->len = gentm->length-2;
		return 1;
	}
	else if (tm->type == V_ASN1_UTCTIME) {
		ASN1_UTCTIME *utctm = (ASN1_UTCTIME *)tm;

		if (utctm->length < 10)
			return 0;
		if (utctm->data[0] >= 0x35)
			return 0;
		if (out->size < utctm->length)
			return -1;

		memcpy(out->str, utctm->data, utctm->length);
		out->len = utctm->length;
		return 1;
	}

	return 0;
}

/* Extract an entry from a X509_NAME and copy its value to an output chunk.
 * Returns 1 if entry found, 0 if entry not found, or -1 if output not large enough.
 */
static int
ssl_sock_get_dn_entry(X509_NAME *a, const struct chunk *entry, int pos, struct chunk *out)
{
	X509_NAME_ENTRY *ne;
	int i, j, n;
	int cur = 0;
	const char *s;
	char tmp[128];

	out->len = 0;
	for (i = 0; i < sk_X509_NAME_ENTRY_num(a->entries); i++) {
		if (pos < 0)
			j = (sk_X509_NAME_ENTRY_num(a->entries)-1) - i;
		else
			j = i;

		ne = sk_X509_NAME_ENTRY_value(a->entries, j);
		n = OBJ_obj2nid(ne->object);
		if ((n == NID_undef) || ((s = OBJ_nid2sn(n)) == NULL)) {
			i2t_ASN1_OBJECT(tmp, sizeof(tmp), ne->object);
			s = tmp;
		}

		if (chunk_strcasecmp(entry, s) != 0)
			continue;

		if (pos < 0)
			cur--;
		else
			cur++;

		if (cur != pos)
			continue;

		if (ne->value->length > out->size)
			return -1;

		memcpy(out->str, ne->value->data, ne->value->length);
		out->len = ne->value->length;
		return 1;
	}

	return 0;

}

/* Extract and format full DN from a X509_NAME and copy result into a chunk
 * Returns 1 if dn entries exits, 0 if no dn entry found or -1 if output is not large enough.
 */
static int
ssl_sock_get_dn_oneline(X509_NAME *a, struct chunk *out)
{
	X509_NAME_ENTRY *ne;
	int i, n, ln;
	int l = 0;
	const char *s;
	char *p;
	char tmp[128];

	out->len = 0;
	p = out->str;
	for (i = 0; i < sk_X509_NAME_ENTRY_num(a->entries); i++) {
		ne = sk_X509_NAME_ENTRY_value(a->entries, i);
		n = OBJ_obj2nid(ne->object);
		if ((n == NID_undef) || ((s = OBJ_nid2sn(n)) == NULL)) {
			i2t_ASN1_OBJECT(tmp, sizeof(tmp), ne->object);
			s = tmp;
		}
		ln = strlen(s);

		l += 1 + ln + 1 + ne->value->length;
		if (l > out->size)
			return -1;
		out->len = l;

		*(p++)='/';
		memcpy(p, s, ln);
		p += ln;
		*(p++)='=';
		memcpy(p, ne->value->data, ne->value->length);
		p += ne->value->length;
	}

	if (!out->len)
		return 0;

	return 1;
}

/***** Below are some sample fetching functions for ACL/patterns *****/

/* boolean, returns true if client cert was present */
static int
smp_fetch_ssl_fc_has_crt(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                         const struct arg *args, struct sample *smp, const char *kw)
{
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	smp->flags = 0;
	smp->type = SMP_T_BOOL;
	smp->data.uint = SSL_SOCK_ST_FL_VERIFY_DONE & conn->xprt_st ? 1 : 0;

	return 1;
}

/* bin, returns serial in a binary chunk */
static int
smp_fetch_ssl_c_serial(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                       const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt = NULL;
	int ret = 0;
	struct chunk *smp_trash;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	/* SSL_get_peer_certificate, it increase X509 * ref count */
	crt = SSL_get_peer_certificate(conn->xprt_ctx);
	if (!crt)
		goto out;

	smp_trash = get_trash_chunk();
	if (ssl_sock_get_serial(crt, smp_trash) <= 0)
		goto out;

	smp->data.str = *smp_trash;
	smp->type = SMP_T_BIN;
	ret = 1;
out:
	if (crt)
		X509_free(crt);
	return ret;
}

/* bin, returns the client certificate's SHA-1 fingerprint (SHA-1 hash of DER-encoded certificate) in a binary chunk */
static int
smp_fetch_ssl_c_sha1(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                     const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt = NULL;
	const EVP_MD *digest;
	int ret = 0;
	struct chunk *smp_trash;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	/* SSL_get_peer_certificate, it increase X509 * ref count */
	crt = SSL_get_peer_certificate(conn->xprt_ctx);
	if (!crt)
		goto out;

	smp_trash = get_trash_chunk();
	digest = EVP_sha1();
	X509_digest(crt, digest, (unsigned char *)smp_trash->str, (unsigned int *)&smp_trash->len);

	smp->data.str = *smp_trash;
	smp->type = SMP_T_BIN;
	ret = 1;
out:
	if (crt)
		X509_free(crt);
	return ret;
}

/*str, returns notafter date in ASN1_UTCTIME format */
static int
smp_fetch_ssl_c_notafter(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                       const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt = NULL;
	int ret = 0;
	struct chunk *smp_trash;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	/* SSL_get_peer_certificate, it increase X509 * ref count */
	crt = SSL_get_peer_certificate(conn->xprt_ctx);
	if (!crt)
		goto out;

	smp_trash = get_trash_chunk();
	if (ssl_sock_get_time(X509_get_notAfter(crt), smp_trash) <= 0)
		goto out;

	smp->data.str = *smp_trash;
	smp->type = SMP_T_STR;
	ret = 1;
out:
	if (crt)
		X509_free(crt);
	return ret;
}

/* str, returns a string of a formatted full dn \C=..\O=..\OU=.. \CN=.. */
static int
smp_fetch_ssl_c_i_dn(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                       const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt = NULL;
	X509_NAME *name;
	int ret = 0;
	struct chunk *smp_trash;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	/* SSL_get_peer_certificate, it increase X509 * ref count */
	crt = SSL_get_peer_certificate(conn->xprt_ctx);
	if (!crt)
		goto out;

	name = X509_get_issuer_name(crt);
	if (!name)
		goto out;

	smp_trash = get_trash_chunk();
	if (args && args[0].type == ARGT_STR) {
		int pos = 1;

		if (args[1].type == ARGT_SINT)
			pos = args[1].data.sint;
		else if (args[1].type == ARGT_UINT)
			pos =(int)args[1].data.uint;

		if (ssl_sock_get_dn_entry(name, &args[0].data.str, pos, smp_trash) <= 0)
			goto out;
	}
	else if (ssl_sock_get_dn_oneline(name, smp_trash) <= 0)
		goto out;

	smp->type = SMP_T_STR;
	smp->data.str = *smp_trash;
	ret = 1;
out:
	if (crt)
		X509_free(crt);
	return ret;
}

/*str, returns notbefore date in ASN1_UTCTIME format */
static int
smp_fetch_ssl_c_notbefore(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                       const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt = NULL;
	int ret = 0;
	struct chunk *smp_trash;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	/* SSL_get_peer_certificate, it increase X509 * ref count */
	crt = SSL_get_peer_certificate(conn->xprt_ctx);
	if (!crt)
		goto out;

	smp_trash = get_trash_chunk();
	if (ssl_sock_get_time(X509_get_notBefore(crt), smp_trash) <= 0)
		goto out;

	smp->data.str = *smp_trash;
	smp->type = SMP_T_STR;
	ret = 1;
out:
	if (crt)
		X509_free(crt);
	return ret;
}

/* str, returns a string of a formatted full dn \C=..\O=..\OU=.. \CN=.. */
static int
smp_fetch_ssl_c_s_dn(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                       const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt = NULL;
	X509_NAME *name;
	int ret = 0;
	struct chunk *smp_trash;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	/* SSL_get_peer_certificate, it increase X509 * ref count */
	crt = SSL_get_peer_certificate(conn->xprt_ctx);
	if (!crt)
		goto out;

	name = X509_get_subject_name(crt);
	if (!name)
		goto out;

	smp_trash = get_trash_chunk();
	if (args && args[0].type == ARGT_STR) {
		int pos = 1;

		if (args[1].type == ARGT_SINT)
			pos = args[1].data.sint;
		else if (args[1].type == ARGT_UINT)
			pos =(int)args[1].data.uint;

		if (ssl_sock_get_dn_entry(name, &args[0].data.str, pos, smp_trash) <= 0)
			goto out;
	}
	else if (ssl_sock_get_dn_oneline(name, smp_trash) <= 0)
		goto out;

	smp->type = SMP_T_STR;
	smp->data.str = *smp_trash;
	ret = 1;
out:
	if (crt)
		X509_free(crt);
	return ret;
}

/* integer, returns true if current session use a client certificate */
static int
smp_fetch_ssl_c_used(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                        const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	/* SSL_get_peer_certificate returns a ptr on allocated X509 struct */
	crt = SSL_get_peer_certificate(conn->xprt_ctx);
	if (crt) {
		X509_free(crt);
	}

	smp->type = SMP_T_BOOL;
	smp->data.uint = (crt != NULL);
	return 1;
}

/* integer, returns the client certificate version */
static int
smp_fetch_ssl_c_version(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                        const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	/* SSL_get_peer_certificate returns a ptr on allocated X509 struct */
	crt = SSL_get_peer_certificate(conn->xprt_ctx);
	if (!crt)
		return 0;

	smp->data.uint = (unsigned int)(1 + X509_get_version(crt));
	X509_free(crt);
	smp->type = SMP_T_UINT;

	return 1;
}

/* str, returns the client certificate sig alg */
static int
smp_fetch_ssl_c_sig_alg(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                        const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt;
	int nid;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	/* SSL_get_peer_certificate increase X509 * ref count  */
	crt = SSL_get_peer_certificate(conn->xprt_ctx);
	if (!crt)
		return 0;

	nid = OBJ_obj2nid((ASN1_OBJECT *)(crt->cert_info->signature->algorithm));

	smp->data.str.str = (char *)OBJ_nid2sn(nid);
	if (!smp->data.str.str) {
		X509_free(crt);
		return 0;
	}

	smp->type = SMP_T_CSTR;
	smp->data.str.len = strlen(smp->data.str.str);
	X509_free(crt);

	return 1;
}

/* str, returns the client certificate key alg */
static int
smp_fetch_ssl_c_key_alg(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                        const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt;
	int nid;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	/* SSL_get_peer_certificate increase X509 * ref count  */
	crt = SSL_get_peer_certificate(conn->xprt_ctx);
	if (!crt)
		return 0;

	nid = OBJ_obj2nid((ASN1_OBJECT *)(crt->cert_info->key->algor->algorithm));

	smp->data.str.str = (char *)OBJ_nid2sn(nid);
	if (!smp->data.str.str) {
		X509_free(crt);
		return 0;
	}

	smp->type = SMP_T_CSTR;
	smp->data.str.len = strlen(smp->data.str.str);
	X509_free(crt);

	return 1;
}

/* boolean, returns true if front conn. transport layer is SSL */
static int
smp_fetch_ssl_fc(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                 const struct arg *args, struct sample *smp, const char *kw)
{
	struct connection *conn = objt_conn(l4->si[0].end);

	smp->type = SMP_T_BOOL;
	smp->data.uint = (conn && conn->xprt == &ssl_sock);
	return 1;
}

/* boolean, returns true if client present a SNI */
static int
smp_fetch_ssl_fc_has_sni(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                         const struct arg *args, struct sample *smp, const char *kw)
{
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
	struct connection *conn = objt_conn(l4->si[0].end);

	smp->type = SMP_T_BOOL;
	smp->data.uint = (conn && conn->xprt == &ssl_sock) &&
		conn->xprt_ctx &&
		SSL_get_servername(conn->xprt_ctx, TLSEXT_NAMETYPE_host_name) != NULL;
	return 1;
#else
	return 0;
#endif
}

/* bin, returns serial in a binary chunk */
static int
smp_fetch_ssl_f_serial(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                       const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt = NULL;
	int ret = 0;
	struct chunk *smp_trash;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	crt = SSL_get_certificate(conn->xprt_ctx);
	if (!crt)
		goto out;

	smp_trash = get_trash_chunk();
	if (ssl_sock_get_serial(crt, smp_trash) <= 0)
		goto out;

	smp->data.str = *smp_trash;
	smp->type = SMP_T_BIN;
	ret = 1;
out:
	return ret;
}
/*str, returns notafter date in ASN1_UTCTIME format */
static int
smp_fetch_ssl_f_notafter(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                       const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt = NULL;
	int ret = 0;
	struct chunk *smp_trash;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	crt = SSL_get_certificate(conn->xprt_ctx);
	if (!crt)
		goto out;

	smp_trash = get_trash_chunk();
	if (ssl_sock_get_time(X509_get_notAfter(crt), smp_trash) <= 0)
		goto out;

	smp->data.str = *smp_trash;
	smp->type = SMP_T_STR;
	ret = 1;
out:
	return ret;
}

/*str, returns notbefore date in ASN1_UTCTIME format */
static int
smp_fetch_ssl_f_notbefore(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                       const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt = NULL;
	int ret = 0;
	struct chunk *smp_trash;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	crt = SSL_get_certificate(conn->xprt_ctx);
	if (!crt)
		goto out;

	smp_trash = get_trash_chunk();
	if (ssl_sock_get_time(X509_get_notBefore(crt), smp_trash) <= 0)
		goto out;

	smp->data.str = *smp_trash;
	smp->type = SMP_T_STR;
	ret = 1;
out:
	return ret;
}

/* integer, returns the frontend certificate version */
static int
smp_fetch_ssl_f_version(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                           const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	/* SSL_get_certificate returns a ptr on an SSL * internal sub struct */
	crt = SSL_get_certificate(conn->xprt_ctx);
	if (!crt)
		return 0;

	smp->data.uint = (unsigned int)(1 + X509_get_version(crt));
	smp->type = SMP_T_UINT;

	return 1;
}

/* str, returns the client certificate sig alg */
static int
smp_fetch_ssl_f_sig_alg(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                        const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt;
	int nid;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	crt = SSL_get_certificate(conn->xprt_ctx);
	if (!crt)
		return 0;

	nid = OBJ_obj2nid((ASN1_OBJECT *)(crt->cert_info->signature->algorithm));

	smp->data.str.str = (char *)OBJ_nid2sn(nid);
	if (!smp->data.str.str)
		return 0;

	smp->type = SMP_T_CSTR;
	smp->data.str.len = strlen(smp->data.str.str);

	return 1;
}

/* str, returns the client certificate key alg */
static int
smp_fetch_ssl_f_key_alg(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                        const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt;
	int nid;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	crt = SSL_get_certificate(conn->xprt_ctx);
	if (!crt)
		return 0;

	nid = OBJ_obj2nid((ASN1_OBJECT *)(crt->cert_info->key->algor->algorithm));

	smp->data.str.str = (char *)OBJ_nid2sn(nid);
	if (!smp->data.str.str)
		return 0;

	smp->type = SMP_T_CSTR;
	smp->data.str.len = strlen(smp->data.str.str);

	return 1;
}

/* str, returns a string of a formatted full dn \C=..\O=..\OU=.. \CN=.. */
static int
smp_fetch_ssl_f_i_dn(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                       const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt = NULL;
	X509_NAME *name;
	int ret = 0;
	struct chunk *smp_trash;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	crt = SSL_get_certificate(conn->xprt_ctx);
	if (!crt)
		goto out;

	name = X509_get_issuer_name(crt);
	if (!name)
		goto out;

	smp_trash = get_trash_chunk();
	if (args && args[0].type == ARGT_STR) {
		int pos = 1;

		if (args[1].type == ARGT_SINT)
			pos = args[1].data.sint;
		else if (args[1].type == ARGT_UINT)
			pos =(int)args[1].data.uint;

		if (ssl_sock_get_dn_entry(name, &args[0].data.str, pos, smp_trash) <= 0)
			goto out;
	}
	else if (ssl_sock_get_dn_oneline(name, smp_trash) <= 0)
		goto out;

	smp->type = SMP_T_STR;
	smp->data.str = *smp_trash;
	ret = 1;
out:
	return ret;
}

/* str, returns a string of a formatted full dn \C=..\O=..\OU=.. \CN=.. */
static int
smp_fetch_ssl_f_s_dn(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                       const struct arg *args, struct sample *smp, const char *kw)
{
	X509 *crt = NULL;
	X509_NAME *name;
	int ret = 0;
	struct chunk *smp_trash;
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || !conn->xprt_ctx || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}

	crt = SSL_get_certificate(conn->xprt_ctx);
	if (!crt)
		goto out;

	name = X509_get_subject_name(crt);
	if (!name)
		goto out;

	smp_trash = get_trash_chunk();
	if (args && args[0].type == ARGT_STR) {
		int pos = 1;

		if (args[1].type == ARGT_SINT)
			pos = args[1].data.sint;
		else if (args[1].type == ARGT_UINT)
			pos =(int)args[1].data.uint;

		if (ssl_sock_get_dn_entry(name, &args[0].data.str, pos, smp_trash) <= 0)
			goto out;
	}
	else if (ssl_sock_get_dn_oneline(name, smp_trash) <= 0)
		goto out;

	smp->type = SMP_T_STR;
	smp->data.str = *smp_trash;
	ret = 1;
out:
	return ret;
}

static int
smp_fetch_ssl_fc_cipher(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                        const struct arg *args, struct sample *smp, const char *kw)
{
	struct connection *conn;

	smp->flags = 0;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || !conn->xprt_ctx || conn->xprt != &ssl_sock)
		return 0;

	smp->data.str.str = (char *)SSL_get_cipher_name(conn->xprt_ctx);
	if (!smp->data.str.str)
		return 0;

	smp->type = SMP_T_CSTR;
	smp->data.str.len = strlen(smp->data.str.str);

	return 1;
}

static int
smp_fetch_ssl_fc_alg_keysize(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                             const struct arg *args, struct sample *smp, const char *kw)
{
	struct connection *conn;

	smp->flags = 0;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || !conn->xprt_ctx || conn->xprt != &ssl_sock)
		return 0;

	if (!SSL_get_cipher_bits(conn->xprt_ctx, (int *)&smp->data.uint))
		return 0;

	smp->type = SMP_T_UINT;

	return 1;
}

static int
smp_fetch_ssl_fc_use_keysize(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                             const struct arg *args, struct sample *smp, const char *kw)
{
	struct connection *conn;

	smp->flags = 0;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || !conn->xprt_ctx || conn->xprt != &ssl_sock)
		return 0;

	smp->data.uint = (unsigned int)SSL_get_cipher_bits(conn->xprt_ctx, NULL);
	if (!smp->data.uint)
		return 0;

	smp->type = SMP_T_UINT;

	return 1;
}

#ifdef OPENSSL_NPN_NEGOTIATED
static int
smp_fetch_ssl_fc_npn(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                     const struct arg *args, struct sample *smp, const char *kw)
{
	struct connection *conn;

	smp->flags = 0;
	smp->type = SMP_T_CSTR;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || !conn->xprt_ctx || conn->xprt != &ssl_sock)
		return 0;

	smp->data.str.str = NULL;
	SSL_get0_next_proto_negotiated(conn->xprt_ctx,
	                                (const unsigned char **)&smp->data.str.str, (unsigned *)&smp->data.str.len);

	if (!smp->data.str.str)
		return 0;

	return 1;
}
#endif

#ifdef OPENSSL_ALPN_NEGOTIATED
static int
smp_fetch_ssl_fc_alpn(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                      const struct arg *args, struct sample *smp, const char *kw)
{
	struct connection *conn;

	smp->flags = 0;
	smp->type = SMP_T_CSTR;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || !conn->xprt_ctx || conn->xprt != &ssl_sock)
		return 0;

	smp->data.str.str = NULL;
	SSL_get0_alpn_negotiated(conn->xprt_ctx,
	                         (const unsigned char **)&smp->data.str.str, (unsigned *)&smp->data.str.len);

	if (!smp->data.str.str)
		return 0;

	return 1;
}
#endif

static int
smp_fetch_ssl_fc_protocol(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                          const struct arg *args, struct sample *smp, const char *kw)
{
	struct connection *conn;

	smp->flags = 0;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || !conn->xprt_ctx || conn->xprt != &ssl_sock)
		return 0;

	smp->data.str.str = (char *)SSL_get_version(conn->xprt_ctx);
	if (!smp->data.str.str)
		return 0;

	smp->type = SMP_T_CSTR;
	smp->data.str.len = strlen(smp->data.str.str);

	return 1;
}

static int
smp_fetch_ssl_fc_session_id(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                            const struct arg *args, struct sample *smp, const char *kw)
{
#if OPENSSL_VERSION_NUMBER > 0x0090800fL
	SSL_SESSION *sess;
	struct connection *conn;

	smp->flags = 0;
	smp->type = SMP_T_CBIN;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || !conn->xprt_ctx || conn->xprt != &ssl_sock)
		return 0;

	sess = SSL_get_session(conn->xprt_ctx);
	if (!sess)
		return 0;

	smp->data.str.str = (char *)SSL_SESSION_get_id(sess, (unsigned int *)&smp->data.str.len);
	if (!smp->data.str.str || !&smp->data.str.len)
		return 0;

	return 1;
#else
	return 0;
#endif
}

static int
smp_fetch_ssl_fc_sni(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                     const struct arg *args, struct sample *smp, const char *kw)
{
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
	struct connection *conn;

	smp->flags = 0;
	smp->type = SMP_T_CSTR;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || !conn->xprt_ctx || conn->xprt != &ssl_sock)
		return 0;

	smp->data.str.str = (char *)SSL_get_servername(conn->xprt_ctx, TLSEXT_NAMETYPE_host_name);
	if (!smp->data.str.str)
		return 0;

	smp->data.str.len = strlen(smp->data.str.str);
	return 1;
#else
	return 0;
#endif
}

/* integer, returns the first verify error in CA chain of client certificate chain. */
static int
smp_fetch_ssl_c_ca_err(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                       const struct arg *args, struct sample *smp, const char *kw)
{
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags = SMP_F_MAY_CHANGE;
		return 0;
	}

	smp->type = SMP_T_UINT;
	smp->data.uint = (unsigned int)SSL_SOCK_ST_TO_CA_ERROR(conn->xprt_st);
	smp->flags = 0;

	return 1;
}

/* integer, returns the depth of the first verify error in CA chain of client certificate chain. */
static int
smp_fetch_ssl_c_ca_err_depth(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                             const struct arg *args, struct sample *smp, const char *kw)
{
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags = SMP_F_MAY_CHANGE;
		return 0;
	}

	smp->type = SMP_T_UINT;
	smp->data.uint = (unsigned int)SSL_SOCK_ST_TO_CAEDEPTH(conn->xprt_st);
	smp->flags = 0;

	return 1;
}

/* integer, returns the first verify error on client certificate */
static int
smp_fetch_ssl_c_err(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                    const struct arg *args, struct sample *smp, const char *kw)
{
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags = SMP_F_MAY_CHANGE;
		return 0;
	}

	smp->type = SMP_T_UINT;
	smp->data.uint = (unsigned int)SSL_SOCK_ST_TO_CRTERROR(conn->xprt_st);
	smp->flags = 0;

	return 1;
}

/* integer, returns the verify result on client cert */
static int
smp_fetch_ssl_c_verify(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                       const struct arg *args, struct sample *smp, const char *kw)
{
	struct connection *conn;

	if (!l4)
		return 0;

	conn = objt_conn(l4->si[0].end);
	if (!conn || conn->xprt != &ssl_sock)
		return 0;

	if (!(conn->flags & CO_FL_CONNECTED)) {
		smp->flags = SMP_F_MAY_CHANGE;
		return 0;
	}

	if (!conn->xprt_ctx)
		return 0;

	smp->type = SMP_T_UINT;
	smp->data.uint = (unsigned int)SSL_get_verify_result(conn->xprt_ctx);
	smp->flags = 0;

	return 1;
}

/* parse the "ca-file" bind keyword */
static int bind_parse_ca_file(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	if (!*args[cur_arg + 1]) {
		if (err)
			memprintf(err, "'%s' : missing CAfile path", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if ((*args[cur_arg + 1] != '/') && global.ca_base)
		memprintf(&conf->ca_file, "%s/%s", global.ca_base, args[cur_arg + 1]);
	else
		memprintf(&conf->ca_file, "%s", args[cur_arg + 1]);

	return 0;
}

/* parse the "ciphers" bind keyword */
static int bind_parse_ciphers(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing cipher suite", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	free(conf->ciphers);
	conf->ciphers = strdup(args[cur_arg + 1]);
	return 0;
}

/* parse the "crt" bind keyword */
static int bind_parse_crt(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	char path[MAXPATHLEN];
	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing certificate location", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if ((*args[cur_arg + 1] != '/' ) && global.crt_base) {
		if ((strlen(global.crt_base) + 1 + strlen(args[cur_arg + 1]) + 1) > MAXPATHLEN) {
			memprintf(err, "'%s' : path too long", args[cur_arg]);
			return ERR_ALERT | ERR_FATAL;
		}
		sprintf(path, "%s/%s",  global.crt_base, args[cur_arg + 1]);
		if (ssl_sock_load_cert(path, conf, px, err) > 0)
			return ERR_ALERT | ERR_FATAL;

		return 0;
	}

	if (ssl_sock_load_cert(args[cur_arg + 1], conf, px, err) > 0)
		return ERR_ALERT | ERR_FATAL;

	return 0;
}

/* parse the "crt-list" bind keyword */
static int bind_parse_crt_list(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing certificate location", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if (ssl_sock_load_cert_list_file(args[cur_arg + 1], conf, px, err) > 0) {
		memprintf(err, "'%s' : %s", args[cur_arg], *err);
		return ERR_ALERT | ERR_FATAL;
	}

	return 0;
}

/* parse the "crl-file" bind keyword */
static int bind_parse_crl_file(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
#ifndef X509_V_FLAG_CRL_CHECK
	if (err)
		memprintf(err, "'%s' : library does not support CRL verify", args[cur_arg]);
	return ERR_ALERT | ERR_FATAL;
#else
	if (!*args[cur_arg + 1]) {
		if (err)
			memprintf(err, "'%s' : missing CRLfile path", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if ((*args[cur_arg + 1] != '/') && global.ca_base)
		memprintf(&conf->crl_file, "%s/%s", global.ca_base, args[cur_arg + 1]);
	else
		memprintf(&conf->crl_file, "%s", args[cur_arg + 1]);

	return 0;
#endif
}

/* parse the "ecdhe" bind keyword keywords */
static int bind_parse_ecdhe(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
#if OPENSSL_VERSION_NUMBER < 0x0090800fL
	if (err)
		memprintf(err, "'%s' : library does not support elliptic curve Diffie-Hellman (too old)", args[cur_arg]);
	return ERR_ALERT | ERR_FATAL;
#elif defined(OPENSSL_NO_ECDH)
	if (err)
		memprintf(err, "'%s' : library does not support elliptic curve Diffie-Hellman (disabled via OPENSSL_NO_ECDH)", args[cur_arg]);
	return ERR_ALERT | ERR_FATAL;
#else
	if (!*args[cur_arg + 1]) {
		if (err)
			memprintf(err, "'%s' : missing named curve", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	conf->ecdhe = strdup(args[cur_arg + 1]);

	return 0;
#endif
}

/* parse the "crt_ignerr" and "ca_ignerr" bind keywords */
static int bind_parse_ignore_err(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	int code;
	char *p = args[cur_arg + 1];
	unsigned long long *ignerr = &conf->crt_ignerr;

	if (!*p) {
		if (err)
			memprintf(err, "'%s' : missing error IDs list", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if (strcmp(args[cur_arg], "ca-ignore-err") == 0)
		ignerr = &conf->ca_ignerr;

	if (strcmp(p, "all") == 0) {
		*ignerr = ~0ULL;
		return 0;
	}

	while (p) {
		code = atoi(p);
		if ((code <= 0) || (code > 63)) {
			if (err)
				memprintf(err, "'%s' : ID '%d' out of range (1..63) in error IDs list '%s'",
				          args[cur_arg], code, args[cur_arg + 1]);
			return ERR_ALERT | ERR_FATAL;
		}
		*ignerr |= 1ULL << code;
		p = strchr(p, ',');
		if (p)
			p++;
	}

	return 0;
}

/* parse the "force-sslv3" bind keyword */
static int bind_parse_force_sslv3(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	conf->ssl_options |= BC_SSL_O_USE_SSLV3;
	return 0;
}

/* parse the "force-tlsv10" bind keyword */
static int bind_parse_force_tlsv10(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	conf->ssl_options |= BC_SSL_O_USE_TLSV10;
	return 0;
}

/* parse the "force-tlsv11" bind keyword */
static int bind_parse_force_tlsv11(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
#if SSL_OP_NO_TLSv1_1
	conf->ssl_options |= BC_SSL_O_USE_TLSV11;
	return 0;
#else
	if (err)
		memprintf(err, "'%s' : library does not support protocol TLSv1.1", args[cur_arg]);
	return ERR_ALERT | ERR_FATAL;
#endif
}

/* parse the "force-tlsv12" bind keyword */
static int bind_parse_force_tlsv12(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
#if SSL_OP_NO_TLSv1_2
	conf->ssl_options |= BC_SSL_O_USE_TLSV12;
	return 0;
#else
	if (err)
		memprintf(err, "'%s' : library does not support protocol TLSv1.2", args[cur_arg]);
	return ERR_ALERT | ERR_FATAL;
#endif
}


/* parse the "no-tls-tickets" bind keyword */
static int bind_parse_no_tls_tickets(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	conf->ssl_options |= BC_SSL_O_NO_TLS_TICKETS;
	return 0;
}


/* parse the "no-sslv3" bind keyword */
static int bind_parse_no_sslv3(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	conf->ssl_options |= BC_SSL_O_NO_SSLV3;
	return 0;
}

/* parse the "no-tlsv10" bind keyword */
static int bind_parse_no_tlsv10(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	conf->ssl_options |= BC_SSL_O_NO_TLSV10;
	return 0;
}

/* parse the "no-tlsv11" bind keyword */
static int bind_parse_no_tlsv11(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	conf->ssl_options |= BC_SSL_O_NO_TLSV11;
	return 0;
}

/* parse the "no-tlsv12" bind keyword */
static int bind_parse_no_tlsv12(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	conf->ssl_options |= BC_SSL_O_NO_TLSV12;
	return 0;
}

/* parse the "npn" bind keyword */
static int bind_parse_npn(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
#ifdef OPENSSL_NPN_NEGOTIATED
	char *p1, *p2;

	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing the comma-delimited NPN protocol suite", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	free(conf->npn_str);

	/* the NPN string is built as a suite of (<len> <name>)* */
	conf->npn_len = strlen(args[cur_arg + 1]) + 1;
	conf->npn_str = calloc(1, conf->npn_len);
	memcpy(conf->npn_str + 1, args[cur_arg + 1], conf->npn_len);

	/* replace commas with the name length */
	p1 = conf->npn_str;
	p2 = p1 + 1;
	while (1) {
		p2 = memchr(p1 + 1, ',', conf->npn_str + conf->npn_len - (p1 + 1));
		if (!p2)
			p2 = p1 + 1 + strlen(p1 + 1);

		if (p2 - (p1 + 1) > 255) {
			*p2 = '\0';
			memprintf(err, "'%s' : NPN protocol name too long : '%s'", args[cur_arg], p1 + 1);
			return ERR_ALERT | ERR_FATAL;
		}

		*p1 = p2 - (p1 + 1);
		p1 = p2;

		if (!*p2)
			break;

		*(p2++) = '\0';
	}
	return 0;
#else
	if (err)
		memprintf(err, "'%s' : library does not support TLS NPN extension", args[cur_arg]);
	return ERR_ALERT | ERR_FATAL;
#endif
}

/* parse the "alpn" bind keyword */
static int bind_parse_alpn(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
#ifdef OPENSSL_ALPN_NEGOTIATED
	char *p1, *p2;

	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing the comma-delimited ALPN protocol suite", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	free(conf->alpn_str);

	/* the ALPN string is built as a suite of (<len> <name>)* */
	conf->alpn_len = strlen(args[cur_arg + 1]) + 1;
	conf->alpn_str = calloc(1, conf->alpn_len);
	memcpy(conf->alpn_str + 1, args[cur_arg + 1], conf->alpn_len);

	/* replace commas with the name length */
	p1 = conf->alpn_str;
	p2 = p1 + 1;
	while (1) {
		p2 = memchr(p1 + 1, ',', conf->alpn_str + conf->alpn_len - (p1 + 1));
		if (!p2)
			p2 = p1 + 1 + strlen(p1 + 1);

		if (p2 - (p1 + 1) > 255) {
			*p2 = '\0';
			memprintf(err, "'%s' : ALPN protocol name too long : '%s'", args[cur_arg], p1 + 1);
			return ERR_ALERT | ERR_FATAL;
		}

		*p1 = p2 - (p1 + 1);
		p1 = p2;

		if (!*p2)
			break;

		*(p2++) = '\0';
	}
	return 0;
#else
	if (err)
		memprintf(err, "'%s' : library does not support TLS ALPN extension", args[cur_arg]);
	return ERR_ALERT | ERR_FATAL;
#endif
}

/* parse the "ssl" bind keyword */
static int bind_parse_ssl(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct listener *l;

	conf->is_ssl = 1;

	if (global.listen_default_ciphers && !conf->ciphers)
		conf->ciphers = strdup(global.listen_default_ciphers);

	list_for_each_entry(l, &conf->listeners, by_bind)
		l->xprt = &ssl_sock;

	return 0;
}

/* parse the "strict-sni" bind keyword */
static int bind_parse_strict_sni(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	conf->strict_sni = 1;
	return 0;
}

/* parse the "verify" bind keyword */
static int bind_parse_verify(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	if (!*args[cur_arg + 1]) {
		if (err)
			memprintf(err, "'%s' : missing verify method", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if (strcmp(args[cur_arg + 1], "none") == 0)
		conf->verify = SSL_VERIFY_NONE;
	else if (strcmp(args[cur_arg + 1], "optional") == 0)
		conf->verify = SSL_VERIFY_PEER;
	else if (strcmp(args[cur_arg + 1], "required") == 0)
		conf->verify = SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
	else {
		if (err)
			memprintf(err, "'%s' : unknown verify method '%s', only 'none', 'optional', and 'required' are supported\n",
			          args[cur_arg], args[cur_arg + 1]);
		return ERR_ALERT | ERR_FATAL;
	}

	return 0;
}

/************** "server" keywords ****************/

/* parse the "ca-file" server keyword */
static int srv_parse_ca_file(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
	if (!*args[*cur_arg + 1]) {
		if (err)
			memprintf(err, "'%s' : missing CAfile path", args[*cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if ((*args[*cur_arg + 1] != '/') && global.ca_base)
		memprintf(&newsrv->ssl_ctx.ca_file, "%s/%s", global.ca_base, args[*cur_arg + 1]);
	else
		memprintf(&newsrv->ssl_ctx.ca_file, "%s", args[*cur_arg + 1]);

	return 0;
}

/* parse the "check-ssl" server keyword */
static int srv_parse_check_ssl(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
	newsrv->check.use_ssl = 1;
	if (global.connect_default_ciphers && !newsrv->ssl_ctx.ciphers)
		newsrv->ssl_ctx.ciphers = strdup(global.connect_default_ciphers);
	return 0;
}

/* parse the "ciphers" server keyword */
static int srv_parse_ciphers(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
	if (!*args[*cur_arg + 1]) {
		memprintf(err, "'%s' : missing cipher suite", args[*cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	free(newsrv->ssl_ctx.ciphers);
	newsrv->ssl_ctx.ciphers = strdup(args[*cur_arg + 1]);
	return 0;
}

/* parse the "crl-file" server keyword */
static int srv_parse_crl_file(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
#ifndef X509_V_FLAG_CRL_CHECK
	if (err)
		memprintf(err, "'%s' : library does not support CRL verify", args[*cur_arg]);
	return ERR_ALERT | ERR_FATAL;
#else
	if (!*args[*cur_arg + 1]) {
		if (err)
			memprintf(err, "'%s' : missing CRLfile path", args[*cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if ((*args[*cur_arg + 1] != '/') && global.ca_base)
		memprintf(&newsrv->ssl_ctx.crl_file, "%s/%s", global.ca_base, args[*cur_arg + 1]);
	else
		memprintf(&newsrv->ssl_ctx.crl_file, "%s", args[*cur_arg + 1]);

	return 0;
#endif
}

/* parse the "crt" server keyword */
static int srv_parse_crt(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
	if (!*args[*cur_arg + 1]) {
		if (err)
			memprintf(err, "'%s' : missing certificate file path", args[*cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if ((*args[*cur_arg + 1] != '/') && global.crt_base)
		memprintf(&newsrv->ssl_ctx.client_crt, "%s/%s", global.ca_base, args[*cur_arg + 1]);
	else
		memprintf(&newsrv->ssl_ctx.client_crt, "%s", args[*cur_arg + 1]);

	return 0;
}

/* parse the "force-sslv3" server keyword */
static int srv_parse_force_sslv3(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
	newsrv->ssl_ctx.options |= SRV_SSL_O_USE_SSLV3;
	return 0;
}

/* parse the "force-tlsv10" server keyword */
static int srv_parse_force_tlsv10(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
	newsrv->ssl_ctx.options |= SRV_SSL_O_USE_TLSV10;
	return 0;
}

/* parse the "force-tlsv11" server keyword */
static int srv_parse_force_tlsv11(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
#if SSL_OP_NO_TLSv1_1
	newsrv->ssl_ctx.options |= SRV_SSL_O_USE_TLSV11;
	return 0;
#else
	if (err)
		memprintf(err, "'%s' : library does not support protocol TLSv1.1", args[*cur_arg]);
	return ERR_ALERT | ERR_FATAL;
#endif
}

/* parse the "force-tlsv12" server keyword */
static int srv_parse_force_tlsv12(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
#if SSL_OP_NO_TLSv1_2
	newsrv->ssl_ctx.options |= SRV_SSL_O_USE_TLSV12;
	return 0;
#else
	if (err)
		memprintf(err, "'%s' : library does not support protocol TLSv1.2", args[*cur_arg]);
	return ERR_ALERT | ERR_FATAL;
#endif
}

/* parse the "no-sslv3" server keyword */
static int srv_parse_no_sslv3(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
	newsrv->ssl_ctx.options |= SRV_SSL_O_NO_SSLV3;
	return 0;
}

/* parse the "no-tlsv10" server keyword */
static int srv_parse_no_tlsv10(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
	newsrv->ssl_ctx.options |= SRV_SSL_O_NO_TLSV10;
	return 0;
}

/* parse the "no-tlsv11" server keyword */
static int srv_parse_no_tlsv11(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
	newsrv->ssl_ctx.options |= SRV_SSL_O_NO_TLSV11;
	return 0;
}

/* parse the "no-tlsv12" server keyword */
static int srv_parse_no_tlsv12(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
	newsrv->ssl_ctx.options |= SRV_SSL_O_NO_TLSV12;
	return 0;
}

/* parse the "no-tls-tickets" server keyword */
static int srv_parse_no_tls_tickets(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
	newsrv->ssl_ctx.options |= SRV_SSL_O_NO_TLS_TICKETS;
	return 0;
}

/* parse the "ssl" server keyword */
static int srv_parse_ssl(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
	newsrv->use_ssl = 1;
	if (global.connect_default_ciphers && !newsrv->ssl_ctx.ciphers)
		newsrv->ssl_ctx.ciphers = strdup(global.connect_default_ciphers);
	return 0;
}

/* parse the "verify" server keyword */
static int srv_parse_verify(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
	if (!*args[*cur_arg + 1]) {
		if (err)
			memprintf(err, "'%s' : missing verify method", args[*cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if (strcmp(args[*cur_arg + 1], "none") == 0)
		newsrv->ssl_ctx.verify = SSL_VERIFY_NONE;
	else if (strcmp(args[*cur_arg + 1], "required") == 0)
		newsrv->ssl_ctx.verify = SSL_VERIFY_PEER;
	else {
		if (err)
			memprintf(err, "'%s' : unknown verify method '%s', only 'none' and 'required' are supported\n",
			          args[*cur_arg], args[*cur_arg + 1]);
		return ERR_ALERT | ERR_FATAL;
	}

	return 0;
}

/* parse the "verifyhost" server keyword */
static int srv_parse_verifyhost(char **args, int *cur_arg, struct proxy *px, struct server *newsrv, char **err)
{
	if (!*args[*cur_arg + 1]) {
		if (err)
			memprintf(err, "'%s' : missing hostname to verify against", args[*cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	newsrv->ssl_ctx.verify_host = strdup(args[*cur_arg + 1]);

	return 0;
}

/* Note: must not be declared <const> as its list will be overwritten.
 * Please take care of keeping this list alphabetically sorted.
 */
static struct sample_fetch_kw_list sample_fetch_keywords = {ILH, {
	{ "ssl_c_ca_err",           smp_fetch_ssl_c_ca_err,       0,                   NULL,    SMP_T_UINT, SMP_USE_L5CLI },
	{ "ssl_c_ca_err_depth",     smp_fetch_ssl_c_ca_err_depth, 0,                   NULL,    SMP_T_UINT, SMP_USE_L5CLI },
	{ "ssl_c_err",              smp_fetch_ssl_c_err,          0,                   NULL,    SMP_T_UINT, SMP_USE_L5CLI },
	{ "ssl_c_i_dn",             smp_fetch_ssl_c_i_dn,         ARG2(0,STR,SINT),    NULL,    SMP_T_STR,  SMP_USE_L5CLI },
	{ "ssl_c_key_alg",          smp_fetch_ssl_c_key_alg,      0,                   NULL,    SMP_T_STR,  SMP_USE_L5CLI },
	{ "ssl_c_notafter",         smp_fetch_ssl_c_notafter,     0,                   NULL,    SMP_T_STR,  SMP_USE_L5CLI },
	{ "ssl_c_notbefore",        smp_fetch_ssl_c_notbefore,    0,                   NULL,    SMP_T_STR,  SMP_USE_L5CLI },
	{ "ssl_c_sig_alg",          smp_fetch_ssl_c_sig_alg,      0,                   NULL,    SMP_T_STR,  SMP_USE_L5CLI },
	{ "ssl_c_s_dn",             smp_fetch_ssl_c_s_dn,         ARG2(0,STR,SINT),    NULL,    SMP_T_STR,  SMP_USE_L5CLI },
	{ "ssl_c_serial",           smp_fetch_ssl_c_serial,       0,                   NULL,    SMP_T_BIN,  SMP_USE_L5CLI },
	{ "ssl_c_sha1",             smp_fetch_ssl_c_sha1,         0,                   NULL,    SMP_T_BIN,  SMP_USE_L5CLI },
	{ "ssl_c_used",             smp_fetch_ssl_c_used,         0,                   NULL,    SMP_T_BOOL, SMP_USE_L5CLI },
	{ "ssl_c_verify",           smp_fetch_ssl_c_verify,       0,                   NULL,    SMP_T_UINT, SMP_USE_L5CLI },
	{ "ssl_c_version",          smp_fetch_ssl_c_version,      0,                   NULL,    SMP_T_UINT, SMP_USE_L5CLI },
	{ "ssl_f_i_dn",             smp_fetch_ssl_f_i_dn,         ARG2(0,STR,SINT),    NULL,    SMP_T_STR,  SMP_USE_L5CLI },
	{ "ssl_f_key_alg",          smp_fetch_ssl_f_key_alg,      0,                   NULL,    SMP_T_STR,  SMP_USE_L5CLI },
	{ "ssl_f_notafter",         smp_fetch_ssl_f_notafter,     0,                   NULL,    SMP_T_STR,  SMP_USE_L5CLI },
	{ "ssl_f_notbefore",        smp_fetch_ssl_f_notbefore,    0,                   NULL,    SMP_T_STR,  SMP_USE_L5CLI },
	{ "ssl_f_sig_alg",          smp_fetch_ssl_f_sig_alg,      0,                   NULL,    SMP_T_STR,  SMP_USE_L5CLI },
	{ "ssl_f_s_dn",             smp_fetch_ssl_f_s_dn,         ARG2(0,STR,SINT),    NULL,    SMP_T_STR,  SMP_USE_L5CLI },
	{ "ssl_f_serial",           smp_fetch_ssl_f_serial,       0,                   NULL,    SMP_T_BIN,  SMP_USE_L5CLI },
	{ "ssl_f_version",          smp_fetch_ssl_f_version,      0,                   NULL,    SMP_T_UINT, SMP_USE_L5CLI },
	{ "ssl_fc",                 smp_fetch_ssl_fc,             0,                   NULL,    SMP_T_BOOL, SMP_USE_L5CLI },
	{ "ssl_fc_alg_keysize",     smp_fetch_ssl_fc_alg_keysize, 0,                   NULL,    SMP_T_UINT, SMP_USE_L5CLI },
	{ "ssl_fc_cipher",          smp_fetch_ssl_fc_cipher,      0,                   NULL,    SMP_T_CSTR, SMP_USE_L5CLI },
	{ "ssl_fc_has_crt",         smp_fetch_ssl_fc_has_crt,     0,                   NULL,    SMP_T_BOOL, SMP_USE_L5CLI },
	{ "ssl_fc_has_sni",         smp_fetch_ssl_fc_has_sni,     0,                   NULL,    SMP_T_BOOL, SMP_USE_L5CLI },
#ifdef OPENSSL_NPN_NEGOTIATED
	{ "ssl_fc_npn",             smp_fetch_ssl_fc_npn,         0,                   NULL,    SMP_T_CSTR, SMP_USE_L5CLI },
#endif
#ifdef OPENSSL_ALPN_NEGOTIATED
	{ "ssl_fc_alpn",            smp_fetch_ssl_fc_alpn,        0,                   NULL,    SMP_T_CSTR, SMP_USE_L5CLI },
#endif
	{ "ssl_fc_protocol",        smp_fetch_ssl_fc_protocol,    0,                   NULL,    SMP_T_CSTR, SMP_USE_L5CLI },
	{ "ssl_fc_use_keysize",     smp_fetch_ssl_fc_use_keysize, 0,                   NULL,    SMP_T_UINT, SMP_USE_L5CLI },
	{ "ssl_fc_session_id",      smp_fetch_ssl_fc_session_id,  0,                   NULL,    SMP_T_CBIN, SMP_USE_L5CLI },
	{ "ssl_fc_sni",             smp_fetch_ssl_fc_sni,         0,                   NULL,    SMP_T_CSTR, SMP_USE_L5CLI },
	{ NULL, NULL, 0, 0, 0 },
}};

/* Note: must not be declared <const> as its list will be overwritten.
 * Please take care of keeping this list alphabetically sorted.
 */
static struct acl_kw_list acl_kws = {ILH, {
	{ "ssl_c_i_dn",             NULL,         pat_parse_str,     pat_match_str     },
	{ "ssl_c_key_alg",          NULL,         pat_parse_str,     pat_match_str     },
	{ "ssl_c_notafter",         NULL,         pat_parse_str,     pat_match_str     },
	{ "ssl_c_notbefore",        NULL,         pat_parse_str,     pat_match_str     },
	{ "ssl_c_sig_alg",          NULL,         pat_parse_str,     pat_match_str     },
	{ "ssl_c_s_dn",             NULL,         pat_parse_str,     pat_match_str     },
	{ "ssl_c_serial",           NULL,         pat_parse_bin,     pat_match_bin     },
	{ "ssl_f_i_dn",             NULL,         pat_parse_str,     pat_match_str     },
	{ "ssl_f_key_alg",          NULL,         pat_parse_str,     pat_match_str     },
	{ "ssl_f_notafter",         NULL,         pat_parse_str,     pat_match_str     },
	{ "ssl_f_notbefore",        NULL,         pat_parse_str,     pat_match_str     },
	{ "ssl_f_sig_alg",          NULL,         pat_parse_str,     pat_match_str     },
	{ "ssl_f_s_dn",             NULL,         pat_parse_str,     pat_match_str     },
	{ "ssl_f_serial",           NULL,         pat_parse_bin,     pat_match_bin     },
	{ "ssl_fc_cipher",          NULL,         pat_parse_str,     pat_match_str     },
#ifdef OPENSSL_NPN_NEGOTIATED
	{ "ssl_fc_npn",             NULL,         pat_parse_str,     pat_match_str     },
#endif
#ifdef OPENSSL_ALPN_NEGOTIATED
	{ "ssl_fc_alpn",            NULL,         pat_parse_str,     pat_match_str     },
#endif
	{ "ssl_fc_protocol",        NULL,         pat_parse_str,     pat_match_str     },
	{ "ssl_fc_sni",             "ssl_fc_sni", pat_parse_str,     pat_match_str     },
	{ "ssl_fc_sni_end",         "ssl_fc_sni", pat_parse_str,     pat_match_end     },
	{ "ssl_fc_sni_reg",         "ssl_fc_sni", pat_parse_reg,     pat_match_reg     },
	{ /* END */ },
}};

/* Note: must not be declared <const> as its list will be overwritten.
 * Please take care of keeping this list alphabetically sorted, doing so helps
 * all code contributors.
 * Optional keywords are also declared with a NULL ->parse() function so that
 * the config parser can report an appropriate error when a known keyword was
 * not enabled.
 */
static struct bind_kw_list bind_kws = { "SSL", { }, {
	{ "alpn",                  bind_parse_alpn,           1 }, /* set ALPN supported protocols */
	{ "ca-file",               bind_parse_ca_file,        1 }, /* set CAfile to process verify on client cert */
	{ "ca-ignore-err",         bind_parse_ignore_err,     1 }, /* set error IDs to ignore on verify depth > 0 */
	{ "ciphers",               bind_parse_ciphers,        1 }, /* set SSL cipher suite */
	{ "crl-file",              bind_parse_crl_file,       1 }, /* set certificat revocation list file use on client cert verify */
	{ "crt",                   bind_parse_crt,            1 }, /* load SSL certificates from this location */
	{ "crt-ignore-err",        bind_parse_ignore_err,     1 }, /* set error IDs to ingore on verify depth == 0 */
	{ "crt-list",              bind_parse_crt_list,       1 }, /* load a list of crt from this location */
	{ "ecdhe",                 bind_parse_ecdhe,          1 }, /* defines named curve for elliptic curve Diffie-Hellman */
	{ "force-sslv3",           bind_parse_force_sslv3,    0 }, /* force SSLv3 */
	{ "force-tlsv10",          bind_parse_force_tlsv10,   0 }, /* force TLSv10 */
	{ "force-tlsv11",          bind_parse_force_tlsv11,   0 }, /* force TLSv11 */
	{ "force-tlsv12",          bind_parse_force_tlsv12,   0 }, /* force TLSv12 */
	{ "no-sslv3",              bind_parse_no_sslv3,       0 }, /* disable SSLv3 */
	{ "no-tlsv10",             bind_parse_no_tlsv10,      0 }, /* disable TLSv10 */
	{ "no-tlsv11",             bind_parse_no_tlsv11,      0 }, /* disable TLSv11 */
	{ "no-tlsv12",             bind_parse_no_tlsv12,      0 }, /* disable TLSv12 */
	{ "no-tls-tickets",        bind_parse_no_tls_tickets, 0 }, /* disable session resumption tickets */
	{ "ssl",                   bind_parse_ssl,            0 }, /* enable SSL processing */
	{ "strict-sni",            bind_parse_strict_sni,     0 }, /* refuse negotiation if sni doesn't match a certificate */
	{ "verify",                bind_parse_verify,         1 }, /* set SSL verify method */
	{ "npn",                   bind_parse_npn,            1 }, /* set NPN supported protocols */
	{ NULL, NULL, 0 },
}};

/* Note: must not be declared <const> as its list will be overwritten.
 * Please take care of keeping this list alphabetically sorted, doing so helps
 * all code contributors.
 * Optional keywords are also declared with a NULL ->parse() function so that
 * the config parser can report an appropriate error when a known keyword was
 * not enabled.
 */
static struct srv_kw_list srv_kws = { "SSL", { }, {
	{ "ca-file",               srv_parse_ca_file,        1, 0 }, /* set CAfile to process verify server cert */
	{ "check-ssl",             srv_parse_check_ssl,      0, 0 }, /* enable SSL for health checks */
	{ "ciphers",               srv_parse_ciphers,        1, 0 }, /* select the cipher suite */
	{ "crl-file",              srv_parse_crl_file,       1, 0 }, /* set certificate revocation list file use on server cert verify */
	{ "crt",                   srv_parse_crt,            1, 0 }, /* set client certificate */
	{ "force-sslv3",           srv_parse_force_sslv3,    0, 0 }, /* force SSLv3 */
	{ "force-tlsv10",          srv_parse_force_tlsv10,   0, 0 }, /* force TLSv10 */
	{ "force-tlsv11",          srv_parse_force_tlsv11,   0, 0 }, /* force TLSv11 */
	{ "force-tlsv12",          srv_parse_force_tlsv12,   0, 0 }, /* force TLSv12 */
	{ "no-sslv3",              srv_parse_no_sslv3,       0, 0 }, /* disable SSLv3 */
	{ "no-tlsv10",             srv_parse_no_tlsv10,      0, 0 }, /* disable TLSv10 */
	{ "no-tlsv11",             srv_parse_no_tlsv11,      0, 0 }, /* disable TLSv11 */
	{ "no-tlsv12",             srv_parse_no_tlsv12,      0, 0 }, /* disable TLSv12 */
	{ "no-tls-tickets",        srv_parse_no_tls_tickets, 0, 0 }, /* disable session resumption tickets */
	{ "ssl",                   srv_parse_ssl,            0, 0 }, /* enable SSL processing */
	{ "verify",                srv_parse_verify,         1, 0 }, /* set SSL verify method */
	{ "verifyhost",            srv_parse_verifyhost,     1, 0 }, /* require that SSL cert verifies for hostname */
	{ NULL, NULL, 0, 0 },
}};

/* transport-layer operations for SSL sockets */
struct xprt_ops ssl_sock = {
	.snd_buf  = ssl_sock_from_buf,
	.rcv_buf  = ssl_sock_to_buf,
	.rcv_pipe = NULL,
	.snd_pipe = NULL,
	.shutr    = NULL,
	.shutw    = ssl_sock_shutw,
	.close    = ssl_sock_close,
	.init     = ssl_sock_init,
};

__attribute__((constructor))
static void __ssl_sock_init(void)
{
	STACK_OF(SSL_COMP)* cm;

	SSL_library_init();
	cm = SSL_COMP_get_compression_methods();
	sk_SSL_COMP_zero(cm);
	sample_register_fetches(&sample_fetch_keywords);
	acl_register_keywords(&acl_kws);
	bind_register_keywords(&bind_kws);
	srv_register_keywords(&srv_kws);
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
