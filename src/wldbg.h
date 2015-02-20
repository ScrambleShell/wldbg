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
#include <sys/signalfd.h>

#include "config.h"

#include "wayland/wayland-util.h"
#include "wldbg-pass.h"
#include "util.h"

struct wldbg_connection;

/* XXX make this private */
struct wldbg {
	int epoll_fd;
	int signals_fd;

	sigset_t handled_signals;
	struct wl_list passes;
	struct wl_list monitored_fds;

	unsigned int resolving_objects : 1;

	struct {
		unsigned int one_by_one	: 1;
		unsigned int running	: 1;
		unsigned int error	: 1;
		unsigned int exit	: 1;
	} flags;

	/* this will be list later */
	struct wldbg_connection *conn;
};

/* XXX make this private */
struct pass {
	struct wldbg_pass wldbg_pass;
	struct wl_list link;
	char *name;
};

struct message {
	/* raw data in message */
	void *data;

	/* size of the message in bytes */
	size_t size;

	/* whether it is a message from server or client */
	enum {
		SERVER,
		CLIENT
	} from;

	/* pointer to connectoin structure */
	struct wldbg_connection *connection;
};

int
wldbg_monitor_fd(struct wldbg *wldbg, int fd,
			int (*dispatch)(int fd, void *data),
			void *data);

#endif /* _WLDBG_H_ */
