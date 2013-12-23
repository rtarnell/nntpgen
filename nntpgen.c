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
#include	<sys/resource.h>

#include	<netinet/in.h>
#include	<netinet/tcp.h>

#include	<stdlib.h>
#include	<stdio.h>
#include	<unistd.h>
#include	<string.h>
#include	<netdb.h>
#include	<errno.h>
#include	<fcntl.h>
#include	<ctype.h>
#include	<assert.h>
#include	<time.h>
#include	<pthread.h>

#include	<ev.h>

#include	"nntpgen.h"
#include	"charq.h"

int		 nconns = 1;
int		 nlines = 10;
char		*msgdomain;
int		 streaming;
char const	*server;
char const	*port;
int		 debug;

#define		DEFAULT_DOMAIN "nntpgen.localhost"

#define		ignore_errno(e) ((e) == EAGAIN || (e) == EINPROGRESS || (e) == EWOULDBLOCK)

typedef struct thread {
	pthread_t	 th_id;
	struct ev_loop	*th_loop;
	int		 th_artnum;
	int		 th_nsend,
			 th_naccept,
			 th_ndefer, 
			 th_nreject,
			 th_nrefuse;
	ev_timer	 th_stats;
	char		 th_article_start[8192],
			 th_article_end[8192];
	size_t		 th_article_start_len,
			 th_article_end_len;
	time_t		 th_last_article;
} thread_t;

thread_t	*threads;
int		 nthreads = 1;

void	 do_thread_stats(struct ev_loop *, ev_timer *w, int);
void	*thread_run(void *);

typedef enum conn_state {
	CN_CONNECTING,
	CN_READ_GREETING,
	CN_RUNNING
} conn_state_t;

typedef struct conn {
	int		 cn_fd;
	int		 cn_num;
	ev_io		 cn_readable;
	ev_io		 cn_writable;
	charq_t		*cn_wrbuf;
	charq_t		*cn_rdbuf;
	conn_state_t	 cn_state;
	int		 cn_cq;
	thread_t	*cn_thread;
} conn_t;

void	conn_read(struct ev_loop *, ev_io *, int);
void	conn_write(struct ev_loop *, ev_io *, int);
void	conn_flush(conn_t *);

void	send_article(conn_t *, char const *);

void	do_stats(struct ev_loop *, ev_timer *w, int);

ev_timer	 stats_timer;
time_t		 start_time;

struct ev_loop	*main_loop;

void	 usage(char const *);

int	nsend, naccept, ndefer, nreject, nrefuse;
pthread_mutex_t	stats_mtx;

void
usage(p)
	char const	*p;
{
	fprintf(stderr,
"usage: %s [-Vh] [-c <conns>] [-n <lines>[-d <domain>] <server[:port]>\n"
"\n"
"    -V                   print version\n"
"    -h                   print this text\n"
"    -d <domain>          use this string for message-id domain\n"
"                         (default: %s)\n"
"    -c <num>             number of connections to open\n"
"                         (default: %d)\n"
"    -t <num>             number of threads to use\n"
"                         (default: %d)\n"
"    -n <lines>           length of each article in lines\n"
"                         (default: %d)\n"
"    -D                   show data sent/received\n"
, p, DEFAULT_DOMAIN, nconns, nthreads, nlines);
}

int
main(ac, av)
	char	**av;
{
int	 c, i, j;
char	*progname = av[0], *p;
struct addrinfo	*res, *r, hints;

	while ((c = getopt(ac, av, "Vhd:c:l:Dt:")) != -1) {
		switch (c) {
		case 'V':
			printf("nntpgen %s\n", PACKAGE_VERSION);
			return 0;

		case 'd':
			free(msgdomain);
			msgdomain = strdup(optarg);
			break;

		case 'c':
			if ((nconns = atoi(optarg)) <= 0) {
				fprintf(stderr, "%s: number of connections must be greater than zero\n",
						progname);
				return 1;
			}
			break;

		case 't':
			if ((nthreads = atoi(optarg)) <= 0) {
				fprintf(stderr, "%s: number of threads must be greater than zero\n",
						progname);
				return 1;
			}
			break;

		case 'l':
			if ((nlines = atoi(optarg)) <= 0) {
				fprintf(stderr, "%s: number of lines must be greater than zero\n",
						progname);
				return 1;
			}
			break;

		case 'D':
			debug++;
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

	bzero(&hints, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	i = getaddrinfo(server, port, &hints, &res);
	if (i) {
		fprintf(stderr, "%s: %s:%s: %s\n",
			progname, server, port, gai_strerror(i));
		return 1;
	}

	pthread_mutex_init(&stats_mtx, NULL);
	main_loop = ev_loop_new(ev_supported_backends());

	threads = xcalloc(nthreads, sizeof(thread_t));
	for (i = 0; i < nthreads; i++) {
	thread_t	*th = &threads[i];
		th->th_loop = ev_loop_new(ev_supported_backends());
		ev_timer_init(&th->th_stats, do_thread_stats, .1, .1);
		th->th_stats.data = th;
	}

	for (i = 0, j = 0; i < nconns; i++, j = ((j == (nthreads-1)) ? 0 : j+1)) {
	conn_t		*conn = xcalloc(1, sizeof(*conn));
	thread_t	*th = &threads[j];
	int		 fl, one = 1;

		conn->cn_num = i + 1;
		conn->cn_thread = th;

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

		if (setsockopt(conn->cn_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == -1) {
			fprintf(stderr, "%s:%s: setsockopt(TCP_NODELAY): %s\n",
				server, port, strerror(errno));
			return 1;
		}

		ev_io_init(&conn->cn_readable, conn_read, conn->cn_fd, EV_READ);
		conn->cn_readable.data = conn;
		ev_io_init(&conn->cn_writable, conn_write, conn->cn_fd, EV_WRITE);
		conn->cn_writable.data = conn;

		conn->cn_rdbuf = cq_new();
		conn->cn_wrbuf = cq_new();

		ev_io_start(th->th_loop, &conn->cn_writable);
	}
		
	freeaddrinfo(res);

	ev_timer_init(&stats_timer, do_stats, 1., 1.);
	ev_timer_start(main_loop, &stats_timer);

	for (i = 0; i < nthreads; i++) {
	thread_t	*th = &threads[i];
		pthread_create(&th->th_id, NULL, thread_run, th);
	}

	time(&start_time);
	ev_run(main_loop, 0);

	return 0;
}

void *
thread_run(p)
	void	*p;
{
thread_t	*th = p;
	ev_timer_start(th->th_loop, &th->th_stats);
	ev_run(th->th_loop, 0);
	return NULL;
}

#define MAX_PENDING 128

void
conn_write(loop, w, revents)
	struct ev_loop	*loop;
	ev_io		*w;
{
conn_t	*cn = w->data;

	if (cn->cn_state == CN_CONNECTING) {
		cn->cn_state = CN_READ_GREETING;
		ev_io_start(loop, &cn->cn_readable);
		ev_io_stop(loop, &cn->cn_writable);
		return;
	}

	assert(cn->cn_state == CN_RUNNING);
	conn_flush(cn);
}

void
conn_check(cn)
	conn_t	*cn;
{
	while (cn->cn_cq < MAX_PENDING) {
	char	ln[256];
	int	n;
		n = snprintf(ln, sizeof(ln), "CHECK <%d.%d@%s>\r\n",
			     rand(), (int) getpid(), msgdomain);
		assert(n >= 0);
		if (debug)
			printf("[%d] -> [%s]\n", cn->cn_num, ln);
		cq_append(cn->cn_wrbuf, ln, n);
		cn->cn_cq++;
	}

	conn_flush(cn);
}

void
conn_flush(cn)
	conn_t	*cn;
{
thread_t	*th = cn->cn_thread;

	if (!cq_len(cn->cn_wrbuf)) {
		ev_io_stop(th->th_loop, &cn->cn_writable);
		return;
	}

	if (cq_write(cn->cn_wrbuf, cn->cn_fd) < 0) {
		if (ignore_errno(errno)) {
			ev_io_start(th->th_loop, &cn->cn_writable);
			return;
		}

		printf("[%d] write error: %s\n",
			cn->cn_num, strerror(errno));
		exit(1);
	}

	ev_io_stop(th->th_loop, &cn->cn_writable);
}

void
conn_read(loop, w, revents)
	struct ev_loop	*loop;
	ev_io		*w;
{
conn_t		*cn = w->data;
thread_t	*th = cn->cn_thread;

	for (;;) {
	char	*ln;
	ssize_t	 n;

		if ((n = cq_read(cn->cn_rdbuf, cn->cn_fd)) == -1) {
			if (ignore_errno(errno))
				return;
			printf("[%d] read error: %s\n",
				cn->cn_num, strerror(errno));
			exit(1);
		}

		while (ln = cq_read_line(cn->cn_rdbuf)) {
		char	*l;
		int	 num;

			if (debug)
				printf("[%d] <- [%s]\n", cn->cn_num, ln);

			if (strlen(ln) < 5) {
				printf("[%d] response too short: [%s]\n",
					cn->cn_num, ln);
				exit(1);
			}

			if (ln[3] != ' ') {
				printf("[%d] invalid response: [%s]\n",
					cn->cn_num, ln);
				exit(1);
			}

			if (!isdigit(ln[0]) || !isdigit(ln[1]) || !isdigit(ln[2])) {
				printf("[%d]: missing numeric: [%s]\n",
					cn->cn_num, ln);
				exit(1);
			}

			num = (ln[2] - '0') 
			    + ((ln[1] - '0') * 10)
			    + ((ln[0] - '0') * 100);
			l = ln + 3;
			while (isspace(*l))
				l++;

			if (cn->cn_state == CN_READ_GREETING) {
				if (num != 200) {
					printf("[%d] access denied: %d [%s]\n",
						cn->cn_num, num, ln);
					exit(1);
				}

				printf("[%d] connected\n", cn->cn_num);
				cn->cn_state = CN_RUNNING;

				goto next;
			}

			switch (num) {
			/*
			 * 238 <msg-id> -- CHECK, send the article
			 * 431 <msg-id> -- CHECK, defer the article
			 * 438 <msg-id> -- CHECK, never send the article
			 * 239 <msg-id> -- TAKETHIS, accepted
			 * 439 <msg-id> -- TAKETHIS, rejected
			 */
			case 238:
				th->th_nsend++;
				cn->cn_cq--;
				send_article(cn, l);
				break;

			case 431:
				th->th_ndefer++;
				cn->cn_cq--;
				break;

			case 438:
				th->th_nrefuse++;
				cn->cn_cq--;
				break;

			case 239:
				th->th_naccept++;
				break;

			case 439:
				th->th_nreject++;
				break;
			}

		next:	;
			conn_check(cn);
			if (cq_len(cn->cn_wrbuf) > 8192)
				conn_flush(cn);
			free(ln);
		}

		conn_flush(cn);
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

void
do_stats(loop, w, revents)
	struct ev_loop	*loop;
	ev_timer	*w;
{
struct rusage	rus;
uint64_t	ct;
time_t		upt = time(NULL) - start_time;

	getrusage(RUSAGE_SELF, &rus);
	ct = (rus.ru_utime.tv_sec * 1000) + (rus.ru_utime.tv_usec / 1000)
	   + (rus.ru_stime.tv_sec * 1000) + (rus.ru_stime.tv_usec / 1000);

	pthread_mutex_lock(&stats_mtx);
	printf("send it: %d/s, refused: %d/s, rejected: %d/s, deferred: %d/s, accepted: %d/s, cpu %.2f%%\n",
		nsend, nrefuse, nreject, ndefer, naccept, (((double)ct / 1000) / upt) * 100);
	nsend = nrefuse = nreject = ndefer = naccept = 0;
	pthread_mutex_unlock(&stats_mtx);
}

void
send_article(cn, msgid)
	conn_t		*cn;
	char const	*msgid;
{
char		 art[512];
int		 n;
struct tm	*tim;
time_t		 now = (long) ev_now(cn->cn_thread->th_loop);
thread_t	*th = cn->cn_thread;

	n = snprintf(art, sizeof(art), "TAKETHIS %s\r\n", msgid);
	cq_append(cn->cn_wrbuf, art, n);

	if (!th->th_article_start[0] || (now - th->th_last_article > 5)) {
		th->th_article_start[0] = 0;
		th->th_article_end[0] = 0;

		snprintf(art, sizeof(art), "Path: %s\r\n", "nntpgen");
		strlcat(th->th_article_start, art, sizeof(th->th_article_start));

		n = snprintf(art, sizeof(art), "From: nntpgen <nntpgen@nntpgen.localhost>\r\n");
		strlcat(th->th_article_start, art, sizeof(th->th_article_start));

		tim = localtime(&now);
		n = strftime(art, sizeof(art), "Date: %a, %d %b %Y %H:%M:%S %z\r\n", tim);
		strlcat(th->th_article_start, art, sizeof(th->th_article_start));

		n = snprintf(art, sizeof(art), "Lines: 1\r\n");
		strlcat(th->th_article_start, art, sizeof(th->th_article_start));

		n = snprintf(art, sizeof(art), "Newsgroups: nntpgen.test\r\n");
		strlcat(th->th_article_start, art, sizeof(th->th_article_start));

		n = snprintf(art, sizeof(art), "\r\nThe data.\r\n.\r\n");
		strlcat(th->th_article_end, art, sizeof(th->th_article_end));

		th->th_article_start_len = strlen(th->th_article_start);
		th->th_article_end_len = strlen(th->th_article_end);
		th->th_last_article = now;
	}

	cq_append(cn->cn_wrbuf, th->th_article_start, th->th_article_start_len);

	n = snprintf(art, sizeof(art), "Message-ID: %s\r\n", msgid);
	cq_append(cn->cn_wrbuf, art, n);

	cq_append(cn->cn_wrbuf, th->th_article_end, th->th_article_end_len);

	conn_flush(cn);
}

void
do_thread_stats(loop, w, revents)
	struct ev_loop	*loop;
	ev_timer	*w;
{
thread_t	*th = w->data;

	pthread_mutex_lock(&stats_mtx);
	nsend += th->th_nsend;
	naccept += th->th_naccept;
	ndefer += th->th_ndefer;
	nreject += th->th_nreject;
	nrefuse += th->th_nrefuse;
	pthread_mutex_unlock(&stats_mtx);

	th->th_nsend = th->th_naccept = th->th_ndefer = th->th_nreject
		= th->th_nrefuse = 0;
}
