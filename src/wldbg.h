/*
 * Copyright (c) 2014 Marek Chalupa
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#ifndef _WLDBG_H_
#define _WLDBG_H_

#include <unistd.h>

#include "config.h"

#include "wayland/wayland-util.h"
#include "wldbg-pass.h"

#ifdef DEBUG

extern int debug;
extern int debug_verbose;

#define vdbg(...) 								\
	do {									\
		if (!debug_verbose) break;					\
		fprintf(stderr, "[%d | %s: %d] ", getpid(),			\
				__FILE__, __LINE__);				\
		fprintf(stderr,	__VA_ARGS__);					\
	} while (0)



#define dbg(...) 								\
	do {									\
		if (!debug) break;						\
		fprintf(stderr, "[%d | %s: %d] ", getpid(),			\
				__FILE__, __LINE__);				\
		fprintf(stderr,	__VA_ARGS__);					\
	} while (0)

#define ifdbg(cond, ...)			\
	do {					\
		if (!debug) break;						\
		if (cond)			\
			dbg(__VA_ARGS__);	\
	} while (0)

#else

#define dbg(...)
#define ifdbg(cond, ...)

#endif /* DEBUG */

struct wldbg {
	struct {
		int fd;
		/* TODO get rid of connection??? */
		struct wl_connection *connection;
	} server;

	struct {
		int fd;
		struct wl_connection *connection;

		/* path to the binary */
		const char *path;
		/* pointer to arguments and number of arguments */
		int argc;
		char * const *argv;

		pid_t pid;
	} client;

	int epoll_fd;
	int signals_fd;

	sigset_t handled_signals;
	struct wl_list passes;

	struct {
		unsigned int one_by_one	: 1;
		unsigned int running	: 1;
		unsigned int error	: 1;
		unsigned int exit	: 1;
	} flags;
};

struct pass {
	struct wldbg_pass wldbg_pass;
	struct wl_list link;
};

struct message {
	void *data;
	size_t size;

	enum {
		SERVER,
		CLIENT
	} from;
};

struct wldbg_fd_callback {
	int fd;
	void *data;
	int (*dispatch)(int fd, void *data);
};

int
wldbg_monitor_fd(struct wldbg *wldbg, int fd,
			struct wldbg_fd_callback *cb);

#endif /* _WLDBG_H_ */
