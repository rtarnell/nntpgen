/* nntpgen: generate dummy news articles */
/* 
 * Copyright (c) 2013 River Tarnell.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#include	<sys/types.h>
#include	<sys/socket.h>

#include	<stdlib.h>
#include	<stdio.h>
#include	<unistd.h>
#include	<string.h>
#include	<netdb.h>
#include	<errno.h>
#include	<fcntl.h>

#include	<ev.h>

#include	"nntpgen.h"
#include	"charq.h"

int		 nconns = 1;
char		*msgdomain;
int		 streaming;
char const	*server;
char const	*port;

#define		DEFAULT_DOMAIN "nntpgen.localhost"

#define		ignore_errno(e) ((e) == EAGAIN || (e) == EINPROGRESS || (e) == EWOULDBLOCK)

typedef enum conn_state {
	CN_CONNECTING,
	CN_READ_GREETING,
	CN_READY
} conn_state_t;

typedef struct conn {
	int		 cn_fd;
	int		 cn_num;
	ev_io		 cn_readable;
	ev_io		 cn_writable;
	charq_t		*cn_wrbuf;
	charq_t		*cn_rdbuf;
	conn_state_t	 cn_state;
} conn_t;

void	conn_read(struct ev_loop *, ev_io *, int);
void	conn_write(struct ev_loop *, ev_io *, int);

struct ev_loop	*loop;

void	 usage(char const *);

void
usage(p)
	char const	*p;
{
	fprintf(stderr,
"usage: %s [-V] [-d <domain>] <server[:port]>\n"
"\n"
"    -V                   print this text\n"
"    -d <domain>          use this string for message-id domain\n"
"                         (default: %s)\n"
"    -c <num>             number of connections to open\n"
"                         (default: %d)\n"
, p, DEFAULT_DOMAIN, nconns);
}

int
main(ac, av)
	char	**av;
{
int	 c, i;
char	*progname = av[0], *p;
struct addrinfo	*res, *r, hints;

	while ((c = getopt(ac, av, "Vd:c:")) != -1) {
		switch (c) {
		case 'V':
			printf("nntpgen %s\n", PACKAGE_VERSION);
			return 0;

		case 'd':
			free(msgdomain);
			msgdomain = strdup(optarg);
			break;

		case 'c':
			nconns = atoi(optarg);
			if (nconns <= 0) {
				fprintf(stderr, "%s: number of connections must be greater than zero\n",
						progname);
				return 1;
			}
			break;

		case 'h':
			usage(av[0]);
			return 0;

		default:
			usage(av[0]);
			return 1;
		}
	}
	ac -= optind;
	av += optind;

	if (!msgdomain)
		msgdomain = strdup(DEFAULT_DOMAIN);

	if (!av[0]) {
		usage(progname);
		return 1;
	}

	server = av[0];
	if (p = index(server, ':')) {
		*p++ = 0;
		port = p;
	} else {
		port = strdup("119");
	}

	loop = EV_DEFAULT;

	bzero(&hints, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	i = getaddrinfo(server, port, &hints, &res);
	if (i) {
		fprintf(stderr, "%s: %s:%s: %s\n",
			progname, server, port, gai_strerror(i));
		return 1;
	}

	for (i = 0; i < nconns; i++) {
	conn_t	*conn = xcalloc(1, sizeof(*conn));
	int	 fl;

		conn->cn_num = i + 1;

		for (r = res; r; r = r->ai_next) {
		char	 sname[NI_MAXHOST];
			if ((conn->cn_fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol)) == -1) {
				fprintf(stderr, "%s:%s: socket: %s\n",
					server, port, strerror(errno));
				return 1;
			}

			if (connect(conn->cn_fd, r->ai_addr, r->ai_addrlen) == -1) {
				getnameinfo(r->ai_addr, r->ai_addrlen, sname, sizeof(sname),
						NULL, 0, NI_NUMERICHOST);
				fprintf(stderr, "%s[%s]:%s: connect: %s\n",
					server, sname, port, strerror(errno));
				if (!r->ai_next)
					return 1;
				close(conn->cn_fd);
				goto next;
			}
			break;

next:			;
		}

		if ((fl = fcntl(conn->cn_fd, F_GETFL, 0)) == -1) {
			fprintf(stderr, "%s:%s: fgetfl: %s\n",
				server, port, strerror(errno));
			return 1;
		}

		if (fcntl(conn->cn_fd, F_SETFL, fl | O_NONBLOCK) == -1) {
			fprintf(stderr, "%s:%s: fsetfl: %s\n",
				server, port, strerror(errno));
			return 1;
		}

		ev_io_init(&conn->cn_readable, conn_read, conn->cn_fd, EV_READ);
		conn->cn_readable.data = conn;
		ev_io_init(&conn->cn_writable, conn_write, conn->cn_fd, EV_WRITE);
		conn->cn_writable.data = conn;

		conn->cn_rdbuf = cq_new();
		conn->cn_wrbuf = cq_new();

		ev_io_start(loop, &conn->cn_writable);
	}
		
	freeaddrinfo(res);

	ev_run(loop, 0);
	return 0;
}

void
conn_write(loop, w, revents)
	struct ev_loop	*loop;
	ev_io		*w;
{
conn_t	*cn = w->data;

	printf("[%d] writable\n", cn->cn_num);

	if (cn->cn_state == CN_CONNECTING) {
		cn->cn_state = CN_READ_GREETING;
		ev_io_start(loop, &cn->cn_readable);
		ev_io_stop(loop, &cn->cn_writable);
	}

	ev_io_stop(loop, w);
}

void
conn_read(loop, w, revents)
	struct ev_loop	*loop;
	ev_io		*w;
{
conn_t	*cn = w->data;
	printf("[%d] readable\n", cn->cn_num);

	for (;;) {
	char	*ln;
	ssize_t	 n;

		if ((n = cq_read(cn->cn_rdbuf, cn->cn_fd)) == -1) {
			if (ignore_errno(errno))
				return;
			printf("[%d] read error: %s\n",
				cn->cn_num, strerror(errno));
		}

		while (ln = cq_read_line(cn->cn_rdbuf)) {
			printf("[%d] read [%s]\n",
				cn->cn_num, ln);
			free(ln);
		}
	}
}

void *
xmalloc(sz)
	size_t	sz;
{
void	*ret = malloc(sz);
	if (!ret) {
		fprintf(stderr, "out of memory\n");
		_exit(1);
	}

	return ret;
}

void *
xcalloc(n, sz)
	size_t	n, sz;
{
void	*ret = calloc(n, sz);
	if (!ret) {
		fprintf(stderr, "out of memory\n");
		_exit(1);
	}

	return ret;
}
