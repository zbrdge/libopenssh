/* $OpenBSD: ssh-keysign.c,v 1.37 2013/05/17 00:13:14 djm Exp $ */
/*
 * Copyright (c) 2002 Markus Friedl.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

#include <fcntl.h>
#include <paths.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xmalloc.h"
#include "log.h"
#include "key.h"
#include "ssh.h"
#include "ssh2.h"
#include "misc.h"
#include "sshbuf.h"
#include "authfile.h"
#include "msg.h"
#include "canohost.h"
#include "pathnames.h"
#include "readconf.h"
#include "uidswap.h"
#include "err.h"

/* XXX readconf.c needs these */
uid_t original_real_uid;

static int
valid_request(struct passwd *pw, char *host, struct sshkey **ret,
    u_char *data, size_t datalen)
{
	struct sshbuf *b;
	struct sshkey *key = NULL;
	u_char type, *pkblob, *sid;
	char *p;
	size_t blen, len;
	char *pkalg, *luser;
	int r, pktype, fail;

	fail = 0;

	if ((b = sshbuf_from(data, datalen)) == NULL)
		fatal("%s: sshbuf_from failed", __func__);

	/* session id, currently limited to SHA1 (20 bytes) or SHA256 (32) */
	if ((r = sshbuf_get_string(b, &sid, &len)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));
	if (len != 20 && len != 32)
		fail++;
	free(sid);

	if ((r = sshbuf_get_u8(b, &type)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));
	if (type != SSH2_MSG_USERAUTH_REQUEST)
		fail++;

	/* server user */
	if ((r = sshbuf_skip_string(b)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));

	/* service */
	if ((r = sshbuf_get_cstring(b, &p, NULL)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));
	if (strcmp("ssh-connection", p) != 0)
		fail++;
	free(p);

	/* method */
	if ((r = sshbuf_get_cstring(b, &p, NULL)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));
	if (strcmp("hostbased", p) != 0)
		fail++;
	free(p);

	/* pubkey */
	if ((r = sshbuf_get_cstring(b, &pkalg, NULL)) != 0 ||
	    (r = sshbuf_get_string(b, &pkblob, &blen)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));

	pktype = sshkey_type_from_name(pkalg);
	if (pktype == KEY_UNSPEC)
		fail++;
	else if ((r = sshkey_from_blob(pkblob, blen, &key)) != 0) {
		error("%s: bad key blob: %s", __func__, ssh_err(r));
		fail++;
	} else if (key->type != pktype)
		fail++;
	free(pkalg);
	free(pkblob);

	/* client host name, handle trailing dot */
	if ((r = sshbuf_get_cstring(b, &p, &len)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));
	debug2("valid_request: check expect chost %s got %s", host, p);
	if (strlen(host) != len - 1)
		fail++;
	else if (p[len - 1] != '.')
		fail++;
	else if (strncasecmp(host, p, len - 1) != 0)
		fail++;
	free(p);

	/* local user */
	if ((r = sshbuf_get_cstring(b, &luser, NULL)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));

	if (strcmp(pw->pw_name, luser) != 0)
		fail++;
	free(luser);

	/* end of message */
	if (sshbuf_len(b) != 0)
		fail++;
	sshbuf_free(b);

	debug3("valid_request: fail %d", fail);

	if (fail && key != NULL)
		sshkey_free(key);
	else
		*ret = key;

	return (fail ? -1 : 0);
}

int
main(int argc, char **argv)
{
	struct sshbuf *b;
	Options options;
#define NUM_KEYTYPES 3
	struct sshkey *keys[NUM_KEYTYPES], *key = NULL;
	struct passwd *pw;
	int r, key_fd[NUM_KEYTYPES], i, found, version = 2, fd;
	u_char *signature, *data, rver;
	char *host;
	size_t slen, dlen;
	u_int32_t rnd[256];

	/* Ensure that stdin and stdout are connected */
	if ((fd = open(_PATH_DEVNULL, O_RDWR)) < 2)
		exit(1);
	/* Leave /dev/null fd iff it is attached to stderr */
	if (fd > 2)
		close(fd);

	i = 0;
	key_fd[i++] = open(_PATH_HOST_DSA_KEY_FILE, O_RDONLY);
	key_fd[i++] = open(_PATH_HOST_ECDSA_KEY_FILE, O_RDONLY);
	key_fd[i++] = open(_PATH_HOST_RSA_KEY_FILE, O_RDONLY);

	original_real_uid = getuid();	/* XXX readconf.c needs this */
	if ((pw = getpwuid(original_real_uid)) == NULL)
		fatal("getpwuid failed");
	pw = pwcopy(pw);

	permanently_set_uid(pw);

#ifdef DEBUG_SSH_KEYSIGN
	log_init("ssh-keysign", SYSLOG_LEVEL_DEBUG3, SYSLOG_FACILITY_AUTH, 0);
#endif

	/* verify that ssh-keysign is enabled by the admin */
	initialize_options(&options);
	(void)read_config_file(_PATH_HOST_CONFIG_FILE, "", &options, 0);
	fill_default_options(&options);
	if (options.enable_ssh_keysign != 1)
		fatal("ssh-keysign not enabled in %s",
		    _PATH_HOST_CONFIG_FILE);

	for (i = found = 0; i < NUM_KEYTYPES; i++) {
		if (key_fd[i] != -1)
			found = 1;
	}
	if (found == 0)
		fatal("could not open any host key");

	OpenSSL_add_all_algorithms();
	for (i = 0; i < 256; i++)
		rnd[i] = arc4random();
	RAND_seed(rnd, sizeof(rnd));

	found = 0;
	for (i = 0; i < NUM_KEYTYPES; i++) {
		keys[i] = NULL;
		if (key_fd[i] == -1)
			continue;
		r = sshkey_load_private_pem(key_fd[i], KEY_UNSPEC,
		    NULL, &(keys[i]), NULL);
		if (r != 0)
			error("Load private: %s", ssh_err(r));
		close(key_fd[i]);
		if (keys[i] != NULL)
			found = 1;
	}
	if (!found)
		fatal("no hostkey found");

	if ((b = sshbuf_new()) == NULL)
		fatal("%s: sshbuf_new failed", __func__);
	if (ssh_msg_recv(STDIN_FILENO, b) < 0)
		fatal("ssh_msg_recv failed");
	if ((r = sshbuf_get_u8(b, &rver)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));
	if (rver != version)
		fatal("bad version: received %d, expected %d", rver, version);
	if ((r = sshbuf_get_u32(b, (u_int *)&fd)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));
	if (fd < 0 || fd == STDIN_FILENO || fd == STDOUT_FILENO)
		fatal("bad fd");
	if ((host = get_local_name(fd)) == NULL)
		fatal("cannot get local name for fd");

	if ((r = sshbuf_get_string(b, &data, &dlen)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));
	if (valid_request(pw, host, &key, data, dlen) < 0)
		fatal("not a valid request");
	free(host);

	found = 0;
	for (i = 0; i < NUM_KEYTYPES; i++) {
		if (keys[i] != NULL &&
		    sshkey_equal_public(key, keys[i])) {
			found = 1;
			break;
		}
	}
	if (!found)
		fatal("no matching hostkey found");

	if ((r = sshkey_sign(keys[i], &signature, &slen, data, dlen, 0)) != 0)
		fatal("sshkey_sign failed: %s", ssh_err(r));
	free(data);

	/* send reply */
	sshbuf_reset(b);
	if ((r = sshbuf_put_string(b, signature, slen)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));
	if (ssh_msg_send(STDOUT_FILENO, version, b) == -1)
		fatal("ssh_msg_send failed");

	return (0);
}
