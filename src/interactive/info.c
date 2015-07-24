/*
 * Copyright (c) 2015 Marek Chalupa
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>

#include "wayland/wayland-private.h"

#include "interactive.h"
#include "util.h"
#include "wldbg-private.h"
#include "resolve.h"
#include "interactive-commands.h"

static void
print_object(uint32_t id, const struct wl_interface *intf, void *data)
{
	(void) data;
	printf("\t%u -> %s\n", id, intf ? intf->name : "NULL");
}

static void
print_objects(struct message *message)
{
	resolved_objects_iterate(message->connection->resolved_objects,
				 print_object, NULL);
}

static void
print_breakpoints(struct wldbg_interactive *wldbgi)
{
	struct breakpoint *b;

	if (wl_list_empty(&wldbgi->breakpoints)) {
		printf("No breakpoints\n");
		return;
	}

	wl_list_for_each(b, &wldbgi->breakpoints, link) {
		printf("%u: break on %s\n", b->id, b->description);
	}
}

static void
info_wldbg(struct wldbg_interactive *wldbgi)
{
	struct wldbg *wldbg = wldbgi->wldbg;

	printf("\n-- Wldbg -- \n");

	printf("Monitored fds num: %d\n", wl_list_length(&wldbg->monitored_fds));
	printf("Resolving objects: %d\n", wldbg->resolving_objects);
	printf("Flags:"
	       "\tone_by_one : %u\n"
	       "\trunning    : %u\n"
	       "\terror      : %u\n"
	       "\texit       : %u\n"
	       "\tserver_mode: %u\n",
	       wldbg->flags.one_by_one,
	       wldbg->flags.running,
	       wldbg->flags.error,
	       wldbg->flags.exit,
	       wldbg->flags.server_mode);

	if (!wldbg->flags.server_mode)
		return;

	printf("Server mode:\n"
		"\told socket name: \'%s\'\n"
		"\told socket path: \'%s\'\n"
		"\twldbg socket path: \'%s\'\n"
		"\twldbg socket path: \'%s\'\n"
		"\tlock address: \'%s\'\n"
		"\tconnect to: \'%s\'\n",
		wldbg->server_mode.old_socket_path,
		wldbg->server_mode.wldbg_socket_path,
		wldbg->server_mode.old_socket_name,
		wldbg->server_mode.wldbg_socket_name,
		wldbg->server_mode.lock_addr,
		wldbg->server_mode.connect_to);

	printf("Connections number: %d\n", wldbg->connections_num);
}

static void
info_connections(struct wldbg_interactive *wldbgi)
{
	struct wldbg *wldbg = wldbgi->wldbg;
	struct wldbg_connection *conn;
	int i;
	int n = 0;

	printf("\n-- Connections -- \n");
	wl_list_for_each(conn, &wldbg->connections, link) {
		++n;

		printf("%d.\n", n);
		printf("\tserver: pid=%d\n", conn->server.pid);
		printf("\tclient: pid=%d\n", conn->client.pid);
		printf("\t      : program=\'%s\'\n", conn->client.program);
		printf("\t      : path=\'%s\'\n", conn->client.path);
		printf("\t      : argc=\'%d\'\n", conn->client.argc);
		for (i = 0; i < conn->client.argc; ++i)
			printf("\t      :   argv[%d]=\'%s\'\n",
			       i, conn->client.argv[i]);

	}
}

int
cmd_info(struct wldbg_interactive *wldbgi,
		struct message *message, char *buf)
{

#define MATCH(buf, str) (strncmp((buf), (str "\n"), (sizeof((str)) + 1)) == 0)

	if (MATCH(buf, "m") || MATCH(buf, "message")) {
		printf("Sender: %s (no. %lu), size: %lu\n",
			message->from == SERVER ? "server" : "client",
			message->from == SERVER ? wldbgi->statistics.server_msg_no
						: wldbgi->statistics.client_msg_no,
			message->size);
	} else if (MATCH(buf, "o") || MATCH(buf, "objects")) {
		print_objects(message);
	} else if (MATCH(buf, "b") || MATCH(buf, "breakpoints")) {
		print_breakpoints(wldbgi);
	} else if (MATCH(buf, "p") || MATCH(buf, "proc")
		   || MATCH(buf, "process")) {
		info_wldbg(wldbgi);
		info_connections(wldbgi);
	} else if (MATCH(buf, "c") || MATCH(buf, "conn")
		   || MATCH(buf, "connection")) {
		info_connections(wldbgi);
	} else {
		printf("Unknown arguments\n");
	}

	return CMD_CONTINUE_QUERY;

#undef MATCH
}

void
cmd_info_help(int oneline)
{
	if (oneline) {
		printf("Show info about entities");
		return;
	}

	printf("info WHAT (i WHAT)\n"
	       "\n"
	       "message (m)\n"
	       "breakpoints (b)\n"
	       "process (proc, p)\n"
	       "connection (conn, c)\n");
}