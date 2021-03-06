/* $OpenBSD: ssh-add.c,v 1.106 2013/05/17 00:13:14 djm Exp $ */
/*
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 * Adds an identity to the authentication server, or removes an identity.
 *
 * As far as I am concerned, the code I have written for this software
 * can be used freely for any purpose.  Any derived versions of this
 * software must be clearly marked as such, and if the derived work is
 * incompatible with the protocol description in the RFC file, it must be
 * called by a name other than "ssh" or "Secure Shell".
 *
 * SSH2 implementation,
 * Copyright (c) 2000, 2001 Markus Friedl.  All rights reserved.
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
#include <sys/stat.h>
#include <sys/param.h>

#include <openssl/evp.h>

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xmalloc.h"
#include "ssh.h"
#include "rsa.h"
#include "log.h"
#include "key.h"
#include "sshbuf.h"
#include "authfd.h"
#include "authfile.h"
#include "pathnames.h"
#include "misc.h"
#include "err.h"

/* argv0 */
extern char *__progname;

/* Default files to add */
static char *default_files[] = {
	_PATH_SSH_CLIENT_ID_RSA,
	_PATH_SSH_CLIENT_ID_DSA,
	_PATH_SSH_CLIENT_ID_ECDSA,
	_PATH_SSH_CLIENT_IDENTITY,
	NULL
};

/* Default lifetime (0 == forever) */
static int lifetime = 0;

/* User has to confirm key use */
static int confirm = 0;

/* we keep a cache of one passphrases */
static char *pass = NULL;
static void
clear_pass(void)
{
	if (pass) {
		memset(pass, 0, strlen(pass));
		free(pass);
		pass = NULL;
	}
}

static int
delete_file(int agent_fd, const char *filename, int key_only)
{
	struct sshkey *public, *cert = NULL;
	char *certpath = NULL, *comment = NULL;
	int r, ret = -1;

	if ((r = sshkey_load_public(filename, &public,  &comment)) != 0) {
		printf("Bad key file %s: %s\n", filename, ssh_err(r));
		return -1;
	}
	if ((r = ssh_remove_identity(agent_fd, public)) == 0) {
		fprintf(stderr, "Identity removed: %s (%s)\n", filename, comment);
		ret = 0;
	} else
		fprintf(stderr, "Could not remove identity \"%s\": %s\n",
		    filename, ssh_err(r));

	if (key_only)
		goto out;

	/* Now try to delete the corresponding certificate too */
	free(comment);
	comment = NULL;
	xasprintf(&certpath, "%s-cert.pub", filename);
	if ((r = sshkey_load_public(certpath, &cert, &comment)) == 0)
		goto out;
	if (!sshkey_equal_public(cert, public))
		fatal("Certificate %s does not match private key %s",
		    certpath, filename);

	if (ssh_remove_identity(agent_fd, cert)) {
		fprintf(stderr, "Identity removed: %s (%s)\n", certpath,
		    comment);
		ret = 0;
	} else
		fprintf(stderr, "Could not remove identity: %s\n", certpath);

 out:
	if (cert != NULL)
		sshkey_free(cert);
	if (public != NULL)
		sshkey_free(public);
	free(certpath);
	free(comment);

	return ret;
}

/* Send a request to remove all identities. */
static int
delete_all(int agent_fd)
{
	int ret = -1;

	if (ssh_remove_all_identities(agent_fd, 1) == 0)
		ret = 0;
	/* ignore error-code for ssh2 */
	/* XXX revisit */
	ssh_remove_all_identities(agent_fd, 2);

	if (ret == 0)
		fprintf(stderr, "All identities removed.\n");
	else
		fprintf(stderr, "Failed to remove all identities.\n");

	return ret;
}

static int
add_file(int agent_fd, const char *filename, int key_only)
{
	struct sshkey *private, *cert;
	char *comment = NULL;
	char msg[1024], *certpath = NULL;
	int r, fd, perms_ok, ret = -1;
	struct sshbuf *keyblob;

	if (strcmp(filename, "-") == 0) {
		fd = STDIN_FILENO;
		filename = "(stdin)";
	} else if ((fd = open(filename, O_RDONLY)) < 0) {
		perror(filename);
		return -1;
	}

	/*
	 * Since we'll try to load a keyfile multiple times, permission errors
	 * will occur multiple times, so check perms first and bail if wrong.
	 */
	if (fd != STDIN_FILENO) {
		perms_ok = sshkey_perm_ok(fd, filename);
		if (!perms_ok) {
			close(fd);
			return -1;
		}
	}
	if ((keyblob = sshbuf_new()) == NULL)
		fatal("%s: sshbuf_new failed", __func__);
	if ((r = sshkey_load_file(fd, filename, keyblob)) != 0) {
		fprintf(stderr, "Error loading key \"%s\": %s\n",
		    filename, ssh_err(r));
		sshbuf_free(keyblob);
		close(fd);
		return -1;
	}
	close(fd);

	/* At first, try empty passphrase */
	r = sshkey_parse_private(keyblob, "", filename, &private, &comment);
	if (r != 0 && r != SSH_ERR_KEY_WRONG_PASSPHRASE) {
		fprintf(stderr, "Error loading key \"%s\": %s\n",
		    filename, ssh_err(r));
		goto fail_load;
	}
	if (comment == NULL)
		comment = xstrdup(filename);
	/* try last */
	if (private == NULL && pass != NULL) {
		r = sshkey_parse_private(keyblob, pass, filename,
		    &private, NULL);
		if (r != 0 && r != SSH_ERR_KEY_WRONG_PASSPHRASE) {
			fprintf(stderr, "Error loading key \"%s\": %s\n",
			    filename, ssh_err(r));
			goto fail_load;
		}
	}
	if (private == NULL) {
		/* clear passphrase since it did not work */
		clear_pass();
		snprintf(msg, sizeof msg, "Enter passphrase for %.200s: ",
		    comment);
		for (;;) {
			pass = read_passphrase(msg, RP_ALLOW_STDIN);
			if (strcmp(pass, "") == 0)
				goto fail_load;
			if ((r = sshkey_parse_private(keyblob, pass, filename,
			    &private, NULL)) == 0)
				break;
			else if (r != SSH_ERR_KEY_WRONG_PASSPHRASE) {
				fprintf(stderr,
				    "Error loading key \"%s\": %s\n",
				    filename, ssh_err(r));
 fail_load:
				clear_pass();
				free(comment);
				sshbuf_free(keyblob);
				return -1;
			}
			clear_pass();
			snprintf(msg, sizeof msg,
			    "Bad passphrase, try again for %.200s: ", comment);
		}
	}
	sshbuf_free(keyblob);

	if ((r = ssh_add_identity_constrained(agent_fd, private, comment,
	    lifetime, confirm)) == 0) {
		fprintf(stderr, "Identity added: %s (%s)\n", filename, comment);
		ret = 0;
		if (lifetime != 0)
			fprintf(stderr,
			    "Lifetime set to %d seconds\n", lifetime);
		if (confirm != 0)
			fprintf(stderr,
			    "The user must confirm each use of the key\n");
	} else {
		fprintf(stderr, "Could not add identity \"%s\": %s\n",
		    filename, ssh_err(r));
	}

	/* Skip trying to load the cert if requested */
	if (key_only)
		goto out;

	/* Now try to add the certificate flavour too */
	xasprintf(&certpath, "%s-cert.pub", filename);
	if ((r = sshkey_load_public(certpath, &cert, NULL)) != 0) {
		if (r != SSH_ERR_SYSTEM_ERROR || errno != ENOENT)
			error("Failed to load certificate \"%s\": %s",
			    certpath, ssh_err(r));
		goto out;
	}

	if (!sshkey_equal_public(cert, private)) {
		error("Certificate %s does not match private key %s",
		    certpath, filename);
		sshkey_free(cert);
		goto out;
	} 

	/* Graft with private bits */
	if ((r = sshkey_to_certified(private,
	    sshkey_cert_is_legacy(cert))) != 0) {
		error("%s: sshkey_to_certified: %s", __func__, ssh_err(r));
		sshkey_free(cert);
		goto out;
	}
	if ((r = sshkey_cert_copy(cert, private)) != 0) {
		error("%s: key_cert_copy: %s", __func__, ssh_err(r));
		sshkey_free(cert);
		goto out;
	}
	sshkey_free(cert);

	if ((r = ssh_add_identity_constrained(agent_fd, private, comment,
	    lifetime, confirm)) != 0) {
		error("Certificate %s (%s) add failed: %s", certpath,
		    private->cert->key_id, ssh_err(r));
		goto out;
	}
	fprintf(stderr, "Certificate added: %s (%s)\n", certpath,
	    private->cert->key_id);
	if (lifetime != 0)
		fprintf(stderr, "Lifetime set to %d seconds\n", lifetime);
	if (confirm != 0)
		fprintf(stderr, "The user must confirm each use of the key\n");
 out:
	free(certpath);
	free(comment);
	sshkey_free(private);

	return ret;
}

static int
update_card(int agent_fd, int add, const char *id)
{
	char *pin;
	int r, ret = -1;

	pin = read_passphrase("Enter passphrase for PKCS#11: ", RP_ALLOW_STDIN);
	if (pin == NULL)
		return -1;

	if ((r = ssh_update_card(agent_fd, add, id, pin, lifetime,
	    confirm)) == 0) {
		fprintf(stderr, "Card %s: %s\n",
		    add ? "added" : "removed", id);
		ret = 0;
	} else {
		fprintf(stderr, "Could not %s card \"%s\": %s\n",
		    add ? "add" : "remove", id, ssh_err(r));
		ret = -1;
	}
	free(pin);
	return ret;
}

static int
list_identities(int agent_fd, int do_fp)
{
	char *fp;
	int version, r, had_identities = 0;
	struct ssh_identitylist *idlist;
	size_t i;

	for (version = 1; version <= 2; version++) {
		if ((r = ssh_fetch_identitylist(agent_fd, version,
		    &idlist)) != 0) {
			if (r != SSH_ERR_AGENT_NO_IDENTITIES)
				fprintf(stderr, "error fetching identities for "
				    "protocol %d: %s\n", version, ssh_err(r));
			continue;
		}
		for (i = 0; i < idlist->nkeys; i++) {
			had_identities = 1;
			if (do_fp) {
				fp = sshkey_fingerprint(idlist->keys[i],
				    SSH_FP_MD5, SSH_FP_HEX);
				printf("%d %s %s (%s)\n",
				    sshkey_size(idlist->keys[i]), fp,
				    idlist->comments[i],
				    sshkey_type(idlist->keys[i]));
				free(fp);
			} else {
				if ((r = sshkey_write(idlist->keys[i],
				    stdout)) != 0) {
					fprintf(stderr, "sshkey_write: %s\n",
					    ssh_err(r));
					continue;
				}
				fprintf(stdout, " %s\n", idlist->comments[i]);
			}
		}
		ssh_free_identitylist(idlist);
	}
	if (!had_identities) {
		printf("The agent has no identities.\n");
		return -1;
	}
	return 0;
}

static int
lock_agent(int agent_fd, int lock)
{
	char prompt[100], *p1, *p2;
	int r, passok = 1, ret = -1;

	strlcpy(prompt, "Enter lock password: ", sizeof(prompt));
	p1 = read_passphrase(prompt, RP_ALLOW_STDIN);
	if (lock) {
		strlcpy(prompt, "Again: ", sizeof prompt);
		p2 = read_passphrase(prompt, RP_ALLOW_STDIN);
		if (strcmp(p1, p2) != 0) {
			fprintf(stderr, "Passwords do not match.\n");
			passok = 0;
		}
		memset(p2, 0, strlen(p2));
		free(p2);
	}
	if (passok) {
		if ((r = ssh_lock_agent(agent_fd, lock, p1)) == 0) {
			fprintf(stderr, "Agent %slocked.\n", lock ? "" : "un");
			ret = 0;
		} else {
			fprintf(stderr, "Failed to %slock agent: %s\n",
			    lock ? "" : "un", ssh_err(r));
		}
	}
	memset(p1, 0, strlen(p1));
	free(p1);
	return (ret);
}

static int
do_file(int agent_fd, int deleting, int key_only, char *file)
{
	if (deleting) {
		if (delete_file(agent_fd, file, key_only) == -1)
			return -1;
	} else {
		if (add_file(agent_fd, file, key_only) == -1)
			return -1;
	}
	return 0;
}

static void
usage(void)
{
	fprintf(stderr, "usage: %s [options] [file ...]\n", __progname);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -l          List fingerprints of all identities.\n");
	fprintf(stderr, "  -L          List public key parameters of all identities.\n");
	fprintf(stderr, "  -k          Load only keys and not certificates.\n");
	fprintf(stderr, "  -c          Require confirmation to sign using identities\n");
	fprintf(stderr, "  -t life     Set lifetime (in seconds) when adding identities.\n");
	fprintf(stderr, "  -d          Delete identity.\n");
	fprintf(stderr, "  -D          Delete all identities.\n");
	fprintf(stderr, "  -x          Lock agent.\n");
	fprintf(stderr, "  -X          Unlock agent.\n");
	fprintf(stderr, "  -s pkcs11   Add keys from PKCS#11 provider.\n");
	fprintf(stderr, "  -e pkcs11   Remove keys provided by PKCS#11 provider.\n");
}

int
main(int argc, char **argv)
{
	extern char *optarg;
	extern int optind;
	int agent_fd;
	char *pkcs11provider = NULL;
	int r, i, ch, deleting = 0, ret = 0, key_only = 0;

	/* Ensure that fds 0, 1 and 2 are open or directed to /dev/null */
	sanitise_stdfd();

	OpenSSL_add_all_algorithms();

	/* First, get a connection to the authentication agent. */
	switch (r = ssh_get_authentication_socket(&agent_fd)) {
	case 0:
		break;
	case SSH_ERR_AGENT_NOT_PRESENT:
		fprintf(stderr, "Could not open a connection to your "
		    "authentication agent.\n");
		exit(2);
	default:
		fprintf(stderr, "Error connecting to agent: %s\n", ssh_err(r));
		exit(2);
	}

	while ((ch = getopt(argc, argv, "klLcdDxXe:s:t:")) != -1) {
		switch (ch) {
		case 'k':
			key_only = 1;
			break;
		case 'l':
		case 'L':
			if (list_identities(agent_fd, ch == 'l' ? 1 : 0) == -1)
				ret = 1;
			goto done;
		case 'x':
		case 'X':
			if (lock_agent(agent_fd, ch == 'x' ? 1 : 0) == -1)
				ret = 1;
			goto done;
		case 'c':
			confirm = 1;
			break;
		case 'd':
			deleting = 1;
			break;
		case 'D':
			if (delete_all(agent_fd) == -1)
				ret = 1;
			goto done;
		case 's':
			pkcs11provider = optarg;
			break;
		case 'e':
			deleting = 1;
			pkcs11provider = optarg;
			break;
		case 't':
			if ((lifetime = convtime(optarg)) == -1) {
				fprintf(stderr, "Invalid lifetime\n");
				ret = 1;
				goto done;
			}
			break;
		default:
			usage();
			ret = 1;
			goto done;
		}
	}
	argc -= optind;
	argv += optind;
	if (pkcs11provider != NULL) {
		if (update_card(agent_fd, !deleting, pkcs11provider) == -1)
			ret = 1;
		goto done;
	}
	if (argc == 0) {
		char buf[MAXPATHLEN];
		struct passwd *pw;
		struct stat st;
		int count = 0;

		if ((pw = getpwuid(getuid())) == NULL) {
			fprintf(stderr, "No user found with uid %u\n",
			    (u_int)getuid());
			ret = 1;
			goto done;
		}

		for (i = 0; default_files[i]; i++) {
			snprintf(buf, sizeof(buf), "%s/%s", pw->pw_dir,
			    default_files[i]);
			if (stat(buf, &st) < 0)
				continue;
			if (do_file(agent_fd, deleting, key_only, buf) == -1)
				ret = 1;
			else
				count++;
		}
		if (count == 0)
			ret = 1;
	} else {
		for (i = 0; i < argc; i++) {
			if (do_file(agent_fd, deleting, key_only,
			    argv[i]) == -1)
				ret = 1;
		}
	}
	clear_pass();

done:
	ssh_close_authentication_socket(agent_fd);
	return ret;
}
