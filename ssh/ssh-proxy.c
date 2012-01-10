/* $OpenBSD: $ */
/*
 * Copyright (c) 2012 Markus Friedl.  All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>

#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ssh_api.h"
#include "xmalloc.h"
#include "log.h"
#include "misc.h"
#include "myproposal.h"
#include "readconf.h"

struct side {
	int fd;
	struct event input, output;
	struct ssh *ssh;
};
struct session {
	struct side client, server;
	int connected;
	TAILQ_ENTRY(session) next;
};
Forward fwd;

void accept_cb(int, short, void *);
void connect_cb(int, short, void *);
void input_cb(int, short, void *);
void output_cb(int, short, void *);

int do_connect(const char *, int);
int do_listen(const char *, int);
void session_close(struct session *);
void ssh_packet_fwd(struct side *, struct side *);
void usage(void);

uid_t original_real_uid;	/* XXX */
TAILQ_HEAD(, session) sessions;
struct kex_params kex_params;

#define BUFSZ 16*1024
char keybuf[BUFSZ];
char known_keybuf[BUFSZ];

int
do_listen(const char *addr, int port)
{
	int sock, on = 1;
	struct addrinfo hints, *ai, *aitop;
	char strport[NI_MAXSERV];
	snprintf(strport, sizeof strport, "%d", port);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(addr, strport, &hints, &aitop) != 0) {
		error("getaddrinfo: fatal error");
		return -1;
	}
	sock = -1;
	for (ai = aitop; ai; ai = ai->ai_next) {
		if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
			continue;
		if ((sock = socket(ai->ai_family, SOCK_STREAM, 0)) < 0)
			continue;
		if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on,
		    sizeof(on)) == -1) {
			error("setsockopt: %s", strerror(errno));
			close(sock);
			sock = -1;
			continue;
		}
		if (bind(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
			error("bind: %s", strerror(errno));
			close(sock);
			sock = -1;
			continue;
		}
		if (listen(sock, 5) < 0) {
			error("listen: %s", strerror(errno));
			close(sock);
			sock = -1;
			continue;
		}
		break;
	}
	freeaddrinfo(aitop);
	return sock;
}

int
do_connect(const char *addr, int port)
{
	struct addrinfo hints, *ai, *aitop;
	int gaierr;
	int sock = -1;
	char strport[NI_MAXSERV];
	snprintf(strport, sizeof strport, "%d", port);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if ((gaierr = getaddrinfo(addr, strport, &hints, &aitop)) != 0) {
		error("getaddrinfo %s: %s", addr, gai_strerror(gaierr));
		return -1;
	}
	for (ai = aitop; ai; ai = ai->ai_next) {
		if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
			continue;
		if ((sock = socket(ai->ai_family, SOCK_STREAM, 0)) < 0) {
			error("socket: %s", strerror(errno));
			continue;
		}
		if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) {
			error("fcntl connect F_SETFL: %s", strerror(errno));
			close(sock);
			sock = -1;
			continue;
		}
		if (connect(sock, ai->ai_addr, ai->ai_addrlen) < 0 &&
		    errno != EINPROGRESS) {
			error("connect(%s, %d): %s", addr, port, strerror(errno));
			close(sock);
			sock = -1;
			continue;
		}
		break;
	}
	freeaddrinfo(aitop);
	if (!ai)
		return -1;
	return sock;
}

void
session_close(struct session *s)
{
	if (s->client.fd != -1)
		close(s->client.fd);
	if (s->server.fd != -1)
		close(s->server.fd);
	if (s->connected == 1) {
		event_del(&s->client.input);
		event_del(&s->client.output);
		event_del(&s->server.input);
		event_del(&s->server.output);
		if (s->client.ssh) {
			ssh_free(s->client.ssh);
			s->client.ssh = NULL;
		}
		if (s->server.ssh) {
			ssh_free(s->server.ssh);
			s->server.ssh = NULL;
		}
		TAILQ_REMOVE(&sessions, s, next);
	}
	xfree(s);
}

void
accept_cb(int fd, short type, void *arg)
{
	struct event *ev = arg;
	struct session *s;
	socklen_t addrlen;
	struct sockaddr addr;

	s = xcalloc(1, sizeof(struct session));
	s->connected = 0;
	s->server.fd = -1;
	s->client.fd = -1;
	s->server.ssh = NULL;
	s->client.ssh = NULL;
	event_add(ev, NULL);
	addrlen = sizeof(addr);
	if ((s->client.fd = accept(fd, &addr, &addrlen)) < 0) {
		fatal("accept: %s", strerror(errno));
		goto fail;
	}
	if (fcntl(s->client.fd, F_SETFL, O_NONBLOCK) < 0) {
		error("fcntl accepted F_SETFL: %s", strerror(errno));
		goto fail;
	}
	if ((s->server.fd = do_connect(fwd.connect_host, fwd.connect_port)) < 0) {
		error("do_connect() failed");
		goto fail;
	}
	event_set(&s->server.output, s->server.fd, EV_WRITE, connect_cb, s);
	event_add(&s->server.output, NULL);
	return;
fail:
	if (s) {
		if (s->client.fd != -1)
			close(s->client.fd);
		xfree(s);
	}
}

void
connect_cb(int fd, short type, void *arg)
{
	struct session *s = arg;
	int soerr;
	socklen_t sz = sizeof(soerr);

	event_del(&s->server.output);
	if (getsockopt(s->server.fd, SOL_SOCKET, SO_ERROR, &soerr, &sz) < 0) {
		soerr = errno;
		error("connect_cb: getsockopt: %s", strerror(errno));
	}
	if (soerr != 0) {
		close(s->server.fd);
		s->server.fd = -1;
		session_close(s);
		return;
	}
	event_set(&s->client.input,  s->client.fd, EV_READ, input_cb, s);
	event_set(&s->client.output, s->client.fd, EV_WRITE, output_cb, s);
	event_set(&s->server.input,  s->server.fd, EV_READ, input_cb, s);
	event_set(&s->server.output, s->server.fd, EV_WRITE, output_cb, s);
	memcpy(kex_params.proposal, myproposal, sizeof(kex_params.proposal));
	s->client.ssh = ssh_init(1, &kex_params);
	s->server.ssh = ssh_init(0, &kex_params);
	if (ssh_add_hostkey(s->client.ssh, keybuf) < 0)
		fatal("could not load server hostkey");
	if (ssh_add_hostkey(s->server.ssh, known_keybuf) < 0)
		fatal("could not load client hostkey");
	event_add(&s->server.input, NULL);
	event_add(&s->client.input, NULL);
	s->connected = 1;
	TAILQ_INSERT_TAIL(&sessions, s, next);
}

void
ssh_packet_fwd(struct side *from, struct side *to)
{
	int type;
	u_char *data;
	u_int len;

	if (!from->ssh || !to->ssh)
		return;
	type = ssh_packet_get(from->ssh);
	if (type) {
		data = ssh_packet_payload(from->ssh, &len);
		debug("ssh_packet_fwd %d->%d type %d len %d",
		    from->fd, to->fd, type, len);
		ssh_packet_put(to->ssh, type, data, len);
	} else {
		debug3("no packet on %d", from->fd);
	}
	ssh_output_ptr(from->ssh, &len);
	if (len) {
		debug3("output %d for %d", len, from->fd);
		event_add(&from->output, NULL);
	}
	ssh_output_ptr(to->ssh, &len);
	if (len) {
		debug3("output %d for %d", len, to->fd);
		event_add(&to->output, NULL);
	}
}

void
input_cb(int fd, short type, void *arg)
{
	u_char buf[BUFSZ];
	struct session *s = arg;
	struct side *r, *w;
	ssize_t len;
	const char *tag;

	if (fd == s->client.fd) {
		tag = "client";
		r = &s->client;
		w = &s->server;
	} else {
		tag = "server";
		r = &s->server;
		w = &s->client;
	}
	debug2("input_cb %s fd %d", tag, fd);
	len = read(fd, buf, sizeof(buf));
	if (len < 0 && (errno == EINTR || errno == EAGAIN)) {
		event_add(&r->input, NULL);
	} else if (len <= 0) {
		debug("read %s failed fd %d len %zd", tag, fd, len);
		session_close(s);
		return;
	} else {
		debug2("read %s fd %d len %zd", tag, fd, len);
		event_add(&r->input, NULL);
		ssh_input_append(r->ssh, buf, len);
	}
	ssh_packet_fwd(r, w);
	ssh_packet_fwd(w, r);
}

void
output_cb(int fd, short type, void *arg)
{
	struct session *s = arg;
	struct side *r, *w;
	ssize_t len;
	const char *tag;
	ssize_t olen;
	char *obuf;

	if (fd == s->client.fd) {
		tag = "client";
		w = &s->client;
		r = &s->server;
	} else {
		tag = "server";
		w = &s->server;
		r = &s->client;
	}
	debug2("output_cb %s fd %d", tag, fd);
	obuf = ssh_output_ptr(w->ssh, (u_int *)&olen);
	if (olen > 0) {
		len = write(fd, obuf, olen);
		if (len < 0 && (errno == EINTR || errno == EAGAIN)) {
			event_add(&w->output, NULL);
		} else if (len <= 0) {
			debug("write %s failed fd %d len %zd", tag, fd, len);
			session_close(s);
			return;
		} else if (len < olen) {
			debug("write %s partial fd %d len %zd olen %zd",
			    tag, fd, len, olen);
			ssh_output_consume(w->ssh, len);
		} else {
			debug2("write %s done fd %d", tag, fd);
			ssh_output_consume(w->ssh, len);
		}
	}
	ssh_packet_fwd(r, w);
	ssh_packet_fwd(w, r);
}

void
usage(void)
{
	extern char *__progname;

	fprintf(stderr,
	    "usage: %s [-dfh] [-L [laddr:]lport:saddr:sport]"
	    " [-C knownkey] [-S serverkey]\n",
	    __progname);
	exit(1);
}

int
main(int argc, char **argv)
{
	int ch, log_stderr = 1, foreground = 0, fd;
	struct event ev;
	ssize_t len;
	char *hostkey_file = NULL, *known_hostkey_file = NULL;
	SyslogFacility log_facility = SYSLOG_FACILITY_AUTH;
	LogLevel log_level = SYSLOG_LEVEL_VERBOSE;
	extern char *__progname;

	TAILQ_INIT(&sessions);

	while ((ch = getopt(argc, argv, "dfC:L:S:")) != -1) {
		switch (ch) {
		case 'd':
			if (log_level == SYSLOG_LEVEL_VERBOSE)
				log_level = SYSLOG_LEVEL_DEBUG1;
			else if (log_level == SYSLOG_LEVEL_DEBUG1)
				log_level = SYSLOG_LEVEL_DEBUG2;
			else if (log_level == SYSLOG_LEVEL_DEBUG2)
				log_level = SYSLOG_LEVEL_DEBUG3;
			break;
		case 'f':
			foreground = 1;
			break;
		case 'C':
			known_hostkey_file = optarg;
			break;
		case 'L':
			if (parse_forward(&fwd, optarg, 0, 0) == 0)
				fatal("cannot parse: %s", optarg);
			if (fwd.listen_host == NULL)
				fwd.listen_host = "0.0.0.0";
			break;
		case 'S':
			hostkey_file = optarg;
			break;
		default:
			usage();
			break;
		}
	}
	log_init(__progname, log_level, log_facility, log_stderr);
	if (hostkey_file) {
		if ((fd = open(hostkey_file, O_RDONLY, 0)) < 0)
			fatal("open: %s %s", hostkey_file, strerror(errno));
		if ((len = read(fd, keybuf, sizeof(keybuf))) < 0)
			fatal("read: %s %s", hostkey_file, strerror(errno));
		keybuf[len] = '\0';
		debug("hk: read %zd bytes", len);
		close(fd);
	}
	if (known_hostkey_file) {
		if ((fd = open(known_hostkey_file, O_RDONLY, 0)) < 0)
			fatal("open: %s %s", hostkey_file, strerror(errno));
		if ((len = read(fd, known_keybuf, sizeof(known_keybuf))) < 0)
			fatal("kh: read: %s %s", hostkey_file, strerror(errno));
		known_keybuf[len] = '\0';
		debug("kh: read %zd bytes", len);
		close(fd);
	}
	if (!foreground)
		daemon(0, 0);
	event_init();
	if ((fd = do_listen(fwd.listen_host, fwd.listen_port)) < 0)
		fatal(" do_listen failed");
	event_set(&ev, fd, EV_READ, accept_cb, &ev);
	event_add(&ev, NULL);
	event_dispatch();
	exit(1);
}
