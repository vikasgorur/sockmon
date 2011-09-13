/*
 * Simple socket program to keep a TCP connection alive
 * Vikas Gorur <vikas@gluster.com>
 */

/*
   Copyright (c) 2010 Gluster, Inc. <http://www.gluster.com>
   This file is part of sockmon.

   sockmon is free software; you can redistribute it and/or modify
   it under the terms of the GNU Affero General Public License as published
   by the Free Software Foundation; either version 3 of the License,
   or (at your option) any later version.

   sockmon is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.
*/

#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>

#include <sys/epoll.h>
#include <pthread.h>

#include <argp.h>

#define		DEFAULT_TIME_INTERVAL		300	/* 5 minutes */

const char *argp_program_version     = "sockmon 1.0";
const char *argp_program_bug_address = "<vikas@gluster.com>";

static char argp_doc[] = "sockmon - A simple client/server that attempts to keep a TCP connection alive indefinitely";

static char argp_args_doc[] = "[-c|-s] -p port";

static struct argp_option options[] = {
	{"port", 'p', "PORT", 0, "Port number to use [Mandatory]"},
	{"client", 'c', "SERVER-IP", 0, "Run as client (connect to SERVER-IP)"},
	{"time", 't', "SECONDS", 0, "Time interval in seconds (default: 300) for pings (client only)"},
	{"server", 's', 0, 0, "Run as server"},
	{ 0 }
};


struct sa_args
{
	char *prog;
	char *port;
	int is_client;
	char *server_addr;
	int is_server;
	char *time;
};


int sa_log (const char *fmt, ...)
{
	va_list      ap;
	time_t       utime = 0;
	struct tm   *tm = NULL;
	char         timestr[256];

        char        *str1 = NULL;
        char        *str2 = NULL;
        char        *msg  = NULL;
        size_t       len  = 0;
        int          ret  = 0;

	utime = time (NULL);
	tm    = localtime (&utime);

	va_start (ap, fmt);

	strftime (timestr, 256, "%Y-%m-%d %H:%M:%S", tm);

	ret = asprintf (&str1, "[%s] ", timestr);

	ret = vasprintf (&str2, fmt, ap);
	va_end (ap);

	len = strlen (str1);
	msg = malloc (len + strlen (str2) + 1);

	strcpy (msg, str1);
	strcpy (msg + len, str2);

	fprintf (stdout, "%s\n", msg);

	free (msg);

        if (str1)
                free (str1);

        if (str2)
                free (str2);

	return (0);
}


void sigpipe_handler (int sig)
{
	sa_log ("ERROR: SIGPIPE received");

	signal (SIGPIPE, sigpipe_handler);
}


// SERVER CODE

/*
 * Return IP:PORT string to identify the peer
 */

char *peerid (int fd)
{
	static char buf[64];

	struct sockaddr_in addr;
	socklen_t addrlen;
	int ret;

	memset (buf, 0, 64);

	addrlen = sizeof (addr);
	ret = getpeername (fd, (struct sockaddr *)&addr, &addrlen);
	if (ret != 0) {
		sa_log ("ERROR: getpeername failed: %s", strerror (errno));
		sprintf (buf, "%s", "ERROR");
		return buf;
	}

	sprintf (buf, "%s:%u", inet_ntoa (addr.sin_addr), addr.sin_port);

	return buf;
}


static pthread_cond_t epoll_cond;
static pthread_mutex_t epoll_cond_mutex;


void *epoll_loop (void *epfd_data)
{
	int epfd = (int)(long) epfd_data;

	const int EVSIZE = 32;

	struct epoll_event events[EVSIZE];

	int nfds;
	int i;
	int ret           = 0;
	const int BUFSIZE = 4096;
	char buf[BUFSIZE];

	char *reply = "pong";
	int reply_len = strlen (reply);

	pthread_mutex_lock (&epoll_cond_mutex);
	// Wait to be signaled before starting epoll wait loop
	pthread_cond_wait (&epoll_cond, &epoll_cond_mutex);

	sa_log ("OK Beginning epoll loop");

	while (1) {
		nfds = epoll_wait (epfd, events, EVSIZE, -1);

		if (nfds < 0) {
			printf ("ERROR: epoll_wait returned < 0!: %s\n", strerror (errno));
			exit (1);
		}

		for (i = 0; i < nfds; i++) {
			int fd = events[i].data.fd;

			if (events[i].events & (EPOLLERR|EPOLLHUP)) {
				sa_log ("ERROR: EPOLLERR|EPOLLHUP from %s", peerid (events[i].data.fd));
				close (fd);
				epoll_ctl (epfd, EPOLL_CTL_DEL, fd, NULL);
			} else if (events[i].events & EPOLLIN) {
				memset (buf, 0, BUFSIZE);

				ret = read (fd, buf, BUFSIZE);

				if (ret == 0) {
					sa_log ("ERROR: read from %s failed: disconnecting",
						peerid (fd));
					close (fd);
					continue;
				}

				sa_log ("OK read from %s = '%s'", peerid (fd), buf);

				ret = write (fd, reply, reply_len);

				if (ret < reply_len) {
					sa_log ("ERROR: write to %s failed: %s",
						peerid (fd), strerror (errno));
					continue;
				}

				sa_log ("OK wrote to %s = '%s'", peerid (fd), reply);
			}
		}
	}
}


static int listen_socket;

void server_cleanup (int sig)
{
	close (listen_socket);
	exit (0);
}


int run_server (struct sa_args *args)
{
	short int port;
	int client;

	struct sockaddr_in server_addr;

	char *endptr;

	char *prog;
	char *server;
	char *port_str;

	int ret;

	pthread_t worker;

	int epfd;
	static struct epoll_event ev;
	int epoll_started = 0;

	struct linger linger;

	prog     = args->prog;
	port_str = args->port;

	port = strtol (port_str, &endptr, 0);
	if (*endptr) {
		printf ("ERROR: invalid port: %s\n", port_str);
		exit (1);
	}

	listen_socket = socket (AF_INET, SOCK_STREAM, 0);
	if (listen_socket < 0) {
		printf ("ERROR: could not create socket: %s\n", strerror (errno));
		exit (1);
	}

	linger.l_linger = 0;
	setsockopt (listen_socket, SOL_SOCKET, SO_LINGER, &linger,
		    sizeof (linger));

	memset (&server_addr, 0, sizeof (server_addr));
	server_addr.sin_family      = AF_INET;
	server_addr.sin_addr.s_addr = htonl (INADDR_ANY);
	server_addr.sin_port        = htons (port);

	ret = bind (listen_socket, (struct sockaddr *) &server_addr, sizeof (server_addr));
	if (ret < 0) {
		printf ("ERROR: could not bind to address: %s\n", strerror (errno));
		exit (1);
	}

	ret = listen (listen_socket, 5);
	if (ret < 0) {
		printf ("ERROR: could not listen: %s\n", strerror (errno));
		exit (1);
	}

	epfd = epoll_create (32);
	if (epfd < 0) {
		printf ("ERROR: could not create epoll fd: %s\n", strerror (errno));
		exit (1);
	}

	ret = pthread_create (&worker, NULL, epoll_loop, (void *)(long) epfd);
	if (ret != 0) {
		printf ("ERROR: could not create worker thread: %s\n", strerror (errno));
		exit (1);
	}

	ret = pthread_mutex_init (&epoll_cond_mutex, NULL);
	if (ret != 0) {
		printf ("ERROR: could not initialize mutex: %s\n", strerror (errno));
		exit (1);
	}

	ret = pthread_cond_init (&epoll_cond, NULL);
	if (ret != 0) {
		printf ("ERROR: could not initialize condition variable: %s\n", strerror (errno));
		exit (1);
	}

	signal (SIGPIPE, SIG_IGN);
	signal (SIGINT, server_cleanup);
	signal (SIGTERM, server_cleanup);

	while (1) {
		sa_log ("OK Listening on port %s", port_str);

		client = accept (listen_socket, NULL, NULL);
		if (client < 0) {
			printf ("ERROR: accept failed: %s\n", strerror (errno));
			exit (1);
		}

		sa_log ("OK Accepted connection from %s", peerid (client));

		ev.events  = EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP;
		ev.data.fd = client;

		ret = epoll_ctl (epfd, EPOLL_CTL_ADD, client, &ev);
		if (ret < 0) {
			printf ("ERROR: epoll_ctl failed: %s\n", strerror (errno));
			exit (1);
		}

		if (epoll_started == 0) {
			pthread_cond_signal (&epoll_cond);
			epoll_started = 1;
		}
	}

	return 0;
}

// CLIENT CODE
void client_loop (struct sockaddr *server_addr, char *prog, char *server,
		  char *port_str, int time)
{
	int sock;
	int ret;

	const int BUFSIZE = 4096;
	char buf[BUFSIZE];

	while (1) {
		/* continuously try to connect and once connected periodically
		   ping the server */

		sock = socket (AF_INET, SOCK_STREAM, 0);
		if (sock < 0) {
			sa_log ("ERROR: Could not create socket: %s", strerror (errno));
			exit (1);
		}

		ret = connect (sock, (struct sockaddr *) server_addr,
			       sizeof (*server_addr));

		if (ret < 0) {
			sa_log ("ERROR: connect to %s:%s failed: %s",
				server, port_str, strerror (errno));
		} else {
			sa_log ("OK Connected to %s:%s",
				server, port_str);
			while (1) {
				char *ping = "ping";
				char pinglen = strlen (ping);

				ret = write (sock, ping, pinglen);
				if (ret != pinglen) {
					sa_log ("ERROR: write to %s:%s failed: %s",
						server, port_str, strerror (errno));
					break;
				}

				ret = read (sock, buf, BUFSIZE);
				if (ret < 0) {
					sa_log ("ERROR: read from %s:%s failed: %s",
						server, port_str, strerror (errno));
					break;
				}

				sa_log ("OK received reply = '%s'", buf);

				sleep (time);
			}
		}

		close (sock);
		sleep (5);
	}
}


int run_client (struct sa_args *args)
{
	int sock;

	struct sockaddr_in server_addr;

	char *endptr;

	char *prog;
	char *server;

	short int port;
	char *port_str;

	int time;
	char *time_str;

	int ret;

	prog     = args->prog;
	server   = args->server_addr;
	port_str = args->port;

	port = strtol (port_str, &endptr, 0);
	if (*endptr) {
		printf ("%s: ERROR: invalid port: %s\n", prog, port_str);
		exit (1);
	}

	time_str = args->time;
	time = strtol (time_str, &endptr, 0);
	if (*endptr) {
		printf ("%s: ERROR: invalid time interval: %s\n", prog, time_str);
		exit (1);
	}

	memset (&server_addr, 0, sizeof (server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port   = htons (port);

	if (inet_aton (server, &server_addr.sin_addr) <= 0) {
		printf ("%s: ERROR: invalid IP address: %s\n", prog, server);
		exit (1);
	}

	signal (SIGPIPE, sigpipe_handler);

	client_loop ((struct sockaddr *)&server_addr, prog, server, port_str, time);

	return 0;
}


static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
	struct sa_args *args = state->input;

	switch (key) {
	case 's':
		args->is_server = 1;
		break;
	case 'c':
		args->is_client = 1;
		args->server_addr = strdup (arg);
		break;
	case 'p':
		args->port = strdup (arg);
		break;
	case 't':
		args->time = strdup (arg);
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}

	return 0;
}


static struct argp argp = {options, parse_opt, argp_args_doc, argp_doc};

int main (int argc, char **argv)
{
	struct sa_args args = {0,};

	args.prog = strdup (argv[0]);
	args.time = "300";

	argp_parse (&argp, argc, argv, 0, 0, &args);

	if (args.is_client && args.is_server) {
		printf ("Can't be both client and server\n");
		printf ("Use --help to see a detailed help message\n");
		exit (1);
	}

	if (args.port == 0) {
		printf ("Port number is mandatory\n");
		printf ("Use --help to see a detailed help message\n");
		exit (1);
	}

	if (!args.is_client && !args.is_server) {
		printf ("Must specify one of -c or -s\n");
		printf ("Use --help to see a detailed help message\n");
		exit (1);
	}

	if (args.is_client) {
		run_client (&args);
	} else if (args.is_server) {
		run_server (&args);
	}

	return 0;
}
