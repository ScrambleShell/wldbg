/*
 * Copyright (c) 2014 - 2015 Marek Chalupa
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>

#include "wayland/wayland-private.h"

#include "wldbg.h"
#include "wldbg-pass.h"
#include "interactive.h"
#include "passes.h"
#include "util.h"
#include "print.h"
#include "wldbg-private.h"
#include "resolve.h"

static unsigned int breakpoint_next_id = 1;

void
terminate_client(struct wldbg_connection *conn)
{
	pid_t pid = conn->client.pid;

	dbg("Terminating client %d\n", pid);

	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
}

int
cmd_quit(struct wldbg_interactive *wldbgi,
		struct message *message,
		char *buf)
{
	int chr;

	(void) message;
	(void) buf;

	if (wldbgi->wldbg->flags.running
		&& !wldbgi->wldbg->flags.error
		&& !wl_list_empty(&wldbgi->wldbg->connections)) {

		printf("Program seems running. "
			"Do you really want to quit? (y)\n");

			chr = getchar();
			if (chr == 'y') {
				wldbg_foreach_connection(wldbgi->wldbg,
							 terminate_client);
			} else {
				/* clear buffer */
				while (getchar() != '\n')
					;

				return CMD_CONTINUE_QUERY;
			}
	}

	dbg("Exiting...\n");

	wldbgi->wldbg->flags.exit = 1;

	return CMD_END_QUERY;
}

int str_to_uint(char *str)
{
	char *num, *numtmp;

	/* skip ws to first digit or newline */
	numtmp = num = skip_ws_to_newline(str);

	if (*num == '\n')
		return -1;

	/* check that it is a number */
	while (!isspace(*numtmp)) {
		if (!isdigit(*numtmp))
			return -1;

		++numtmp;
	}

	return atoi(num);
}

static int
break_on_side(struct message *msg, struct breakpoint *b)
{
	if (msg->from == b->small_data)
		return 1;

	return 0;
}

static int
break_on_id(struct message *msg, struct breakpoint *b)
{
	uint32_t *p = msg->data;

	if (*p == b->small_data)
		return 1;

	return 0;
}

static int
break_on_name(struct message *msg, struct breakpoint *b)
{
	uint32_t *p = msg->data;
	uint32_t id, opcode, bopcode;
	const struct wl_interface *intf;
	const struct wl_message *bmessage, *wl_message = NULL;
	struct resolved_objects *ro = msg->connection->resolved_objects;


	id = p[0];
	opcode = p[1] & 0xffff;
	bopcode = b->small_data;
	bmessage = b->data;

	/* we know that if the opcodes differ, we can
	 * break without any interface checking */
	if (bopcode != opcode)
		return 0;

	intf = resolved_objects_get(ro, id);
	if (!intf)
		return 0;

	if (msg->from == CLIENT) {
		if ((int) opcode < intf->method_count)
			wl_message = &intf->methods[opcode];
	} else {
		if ((int) opcode < intf->event_count)
			wl_message = &intf->events[opcode];
	}

	if (wl_message && bmessage == wl_message)
		return 1;

	return 0;
}

static struct breakpoint *
create_breakpoint(struct resolved_objects *ro, char *buf)
{
	int id, i, opcode;
	struct breakpoint *b;
	const struct wl_interface *intf = NULL;
	char *at;

	b= calloc(1, sizeof *b);
	if (!b) {
		fprintf(stderr, "Out of memory\n");
		return NULL;
	}

	b->id = breakpoint_next_id;

	/* parse buf and find out what the breakpoint
	 * should be */
	if (strcmp(buf, "server\n") == 0) {
		b->applies = break_on_side;
		b->description = strdup("message from server");
		b->small_data = SERVER;
	} else if (strcmp(buf, "client\n") == 0) {
		b->applies = break_on_side;
		b->description = strdup("message from client");
		b->small_data = CLIENT;
	} else if (strncmp(buf, "id ", 3) == 0) {
		id = str_to_uint(buf + 3);
		if (id == -1) {
			printf("Wrong id\n");
			goto err;
		}

		b->applies = break_on_id;
		b->description = strdup("break on id XXX");
		snprintf(b->description + 12, 4, "%d", id);
		b->small_data = id;

	} else {
		if ((at = strchr(buf, '@'))) {
			/* split the string on '@' */
			*at = 0;
			intf = resolved_objects_get_interface(ro, buf);
			if (!intf) {
				printf("Unknown interface\n");
				goto err;
			}

			/* find the request/event with name pointed
			 * by at + 1 */
			++at;
			at[strlen(at) - 1] = 0; /* delete newline */
			opcode = -1;

			/* XXX what if interface has method and event
			 * with the same name? Handle it... */
			for (i = 0; i < intf->method_count; ++i)
				if (strcmp(intf->methods[i].name, at) == 0) {
					b->data = (struct wl_message *) &intf->methods[i];
					opcode = i;
				}

			if (opcode == -1)
				for (i = 0; i < intf->event_count; ++i)
					if (strcmp(intf->events[i].name, at) == 0) {
						b->data = (struct wl_message *) &intf->events[i];
						opcode = i;
					}

			if (opcode == -1) {
				printf("Couldn't find method/event name\n");
				goto err;
			}

			b->small_data = opcode;
			b->applies = break_on_name;
			b->description = malloc(1024);
			if (!b->description) {
				printf("No memory\n");
				goto err;
			}

			snprintf(b->description, 1024, "break on %s@%s",
				 intf->name, ((struct wl_message *) b->data)->name);
		} else {
			printf("Wrong syntax\n");
			goto err;
		}
	}

	++breakpoint_next_id;
	dbg("Created breakpoint %u\n", b->id);

	return b;
err:
	free(b);
	return NULL;
}

void
free_breakpoint(struct breakpoint *b)
{
	assert(b);

	if (b->data_destr && b->data)
		b->data_destr(b->data);
	if (b->description)
		free(b->description);
	free(b);
}

static void
delete_breakpoint(char *buf, struct wldbg_interactive *wldbgi)
{
	int id;
	struct breakpoint *b, *tmp;

	id = str_to_uint(buf);
	if (id == -1) {
		printf("Need valid id\n");
		return;
	}

	wl_list_for_each_safe(b, tmp, &wldbgi->breakpoints, link) {
		if (b->id == (unsigned) id) {
			wl_list_remove(&b->link);
			free_breakpoint(b);

			return;
		}
	}

	printf("Haven't found breakpoint with id %u\n", id);
	return;
}

static int
cmd_break(struct wldbg_interactive *wldbgi,
	  struct message *message,
	  char *buf)
{
	struct breakpoint *b;
	struct resolved_objects *ro = message->connection->resolved_objects;

	(void) message;

	if (strncmp(buf, "delete ", 7) == 0) {
		delete_breakpoint(buf + 7, wldbgi);
		return CMD_CONTINUE_QUERY;
	} else if (strncmp(buf, "d ", 2) == 0) {
		delete_breakpoint(buf + 2, wldbgi);
		return CMD_CONTINUE_QUERY;
	}

	b = create_breakpoint(ro, buf);
	if (b) {
		wl_list_insert(wldbgi->breakpoints.next, &b->link);
		printf("created breakpoint %u\n", b->id);
	}

	return CMD_CONTINUE_QUERY;
}

static void
cmd_pass_help(int oneline)
{
	if (oneline) {
		printf("Add, remove, list passes\n");
		return;
	}

	printf("Possible arguments:\n");
	printf("\tlist\t\t- list available passes\n");
	printf("\tloaded\t\t- list loaded passes\n");
	printf("\tadd NAME\t- add pass NAME.so\n");
	printf("\tremove NAME\t- remove pass NAME\n");
}

static void
add_pass(struct wldbg *wldbg, const char *name)
{
	struct pass *pass;

	dbg("Adding pass '%s'\n", name);

	pass = create_pass(name);
	if (pass) {
		/* XXX add arguments */
		if (pass_init(wldbg, pass, 0, NULL) != 0) {
			fprintf(stderr, "Failed initializing pass '%s'\n",
				name);
			dealloc_pass(pass);
		} else {
			/* insert always at the head */
			wl_list_insert(wldbg->passes.next, &pass->link);

			dbg("Added pass '%s'\n", name);
		}
	} else {
		fprintf(stderr, "Failed adding pass '%s'\n", name);
	}
}

static void
loaded_passes(struct wldbg *wldbg)
{
	struct pass *pass;

	printf("Loaded passes:\n");
	wl_list_for_each(pass, &wldbg->passes, link) {
		printf("\t - %s\n", pass->name);
	}
}

static void
remove_pass(struct wldbg *wldbg, const char *name)
{
	struct pass *pass, *tmp;

	dbg("Removing pass '%s'\n", name);

	wl_list_for_each_safe(pass, tmp, &wldbg->passes, link) {
		if (strcmp(pass->name, name) == 0) {
			wl_list_remove(&pass->link);

			free(pass->name);
			free(pass);

			dbg("Removed pass '%s'\n", name);
			return;
		}
	}

	fprintf(stderr, "Didn't find pass '%s'\n", name);
}



static int
cmd_pass(struct wldbg_interactive *wldbgi,
		struct message *message,
		char *buf)
{
	(void) message;
	(void) buf;

	if (strncmp(buf, "list\n", 5) == 0) {
		list_passes(1);
	} else if (strncmp(buf, "loaded\n", 5) == 0) {
		loaded_passes(wldbgi->wldbg);
	} else if (strncmp(buf, "add ", 4) == 0) {
		/* remove \n from it */
		buf[strlen(buf) - 1] = '\0';
		add_pass(wldbgi->wldbg, buf + 4);
	} else if (strncmp(buf, "remove ", 7) == 0) {
		buf[strlen(buf) - 1] = '\0';
		remove_pass(wldbgi->wldbg, buf + 7);
	} else
		cmd_pass_help(0);

	return CMD_CONTINUE_QUERY;
}

void print_object(uint32_t id, const struct wl_interface *intf, void *data)
{
	(void) data;
	printf("\t%u -> %s\n", id, intf ? intf->name : "NULL");
}

void
print_objects(struct message *message)
{
	resolved_objects_interate(message->connection->resolved_objects,
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
		printf("%d: break on %s\n", b->id, b->description);
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

static int
cmd_info(struct wldbg_interactive *wldbgi,
		struct message *message, char *buf)
{

#define MATCH(buf, str) (strncmp((buf), (str "\n"), (sizeof((str)) + 1)) == 0)

	if (MATCH(buf, "message")) {
		printf("Sender: %s (no. %lu), size: %lu\n",
			message->from == SERVER ? "server" : "client",
			message->from == SERVER ? wldbgi->statistics.server_msg_no
						: wldbgi->statistics.client_msg_no,
			message->size);
	} else if (MATCH(buf, "objects")) {
		print_objects(message);
	} else if (MATCH(buf, "breakpoints")
		   || MATCH(buf, "b")) {
		print_breakpoints(wldbgi);
	} else if (MATCH(buf, "proc")) {
		info_wldbg(wldbgi);
		info_connections(wldbgi);
	} else if (MATCH(buf, "conn")) {
		info_connections(wldbgi);
	} else {
		printf("Unknown arguments\n");
	}

	return CMD_CONTINUE_QUERY;

#undef MATCH
}

static int
cmd_next(struct wldbg_interactive *wldbgi,
		struct message *message,
		char *buf)
{
	(void) message;
	(void) buf;

	if (!wldbgi->wldbg->flags.running) {
		printf("Client is not running\n");
		return CMD_CONTINUE_QUERY;
	}

	wldbgi->stop = 1;
	return CMD_END_QUERY;
}

static int
cmd_continue(struct wldbg_interactive *wldbgi,
		struct message *message,
		char *buf)
{
	(void) message;
	(void) buf;

	if (!wldbgi->wldbg->flags.running) {
		printf("Client is not running\n");
		return CMD_CONTINUE_QUERY;
	}

	return CMD_END_QUERY;
}

static int
cmd_help(struct wldbg_interactive *wldbgi,
		struct message *message, char *buf);

static void
cmd_help_help(int ol)
{
	if (ol)
		printf("Show this help message");
	else
		printf("Print help message. Given argument 'all', print "
			"comprehensive help about all commands.");
}

static int
cmd_send(struct wldbg_interactive *wldbgi,
		struct message *message,
		char *buf)
{
	struct wl_connection *conn;
	uint32_t buffer[1024]; /* size of wl_connection buffer */
	uint32_t size, opcode;
	int where, interactive, i = 0;

	(void) message;
	(void) buf;
	(void) wldbgi;

	if (strncmp(buf, "server", 6) == 0
		|| (buf[0] == 's' && isspace(buf[1]))) {
		where = SERVER;
	} else if (strncmp(buf, "client", 6) == 0
		|| (buf[0] == 'c' && isspace(buf[1]))) {
		where = CLIENT;
	} else {
		printf(" :: send [server|s|client|c]"
			"[message - NOT IMPLEMENTED YET]\n");
		return CMD_CONTINUE_QUERY;
	}

	interactive = 1;

	if (where == SERVER)
		conn = message->connection->server.connection;
	else
		conn = message->connection->client.connection;

	/* XXX later do translation from interface@obj.request etc.. */
	if (interactive) {
		printf("Id: ");
		scanf("%u", &buffer[0]);

		printf("Opcode: ");
		scanf("%u", &opcode);
		i = 2;
	}

	while(scanf("%x", &buffer[i]) != -1)
		++i;

	if (interactive) {
		size = i * sizeof(uint32_t);
		buffer[1] = (size << 16) | (opcode & 0xffff);
	} else {
		/* for debug */
		opcode = buffer[1] & 0xffff;
		size = buffer[1] >> 16;
	}

	dbg("Sending id %u, opcode %u , size %u\n", buffer[0], opcode, size);

	wl_connection_write(conn, buffer, size);

	return CMD_CONTINUE_QUERY;
}

static char *
store_message_to_tmpfile(struct message *message)
{
	int fd, ret;
	/* I don't suppose anybody would attack this program,
	 * so use dangerous function */
	char *file = tempnam(NULL, "wldbg-msg");
	if (!file) {
		perror("Creating tmp file for storing a message");
		return NULL;
	}

	vdbg("Created %s for storing message\n", file);

	fd = open(file, O_WRONLY | O_CREAT | O_EXCL, 0700);
	if (!fd) {
		perror("Opening tmp file for writing");
		free(file);
		return NULL;
	}

	ret = write(fd, message->data, message->size);
	if ((size_t) ret != message->size) {
		if (ret < 0)
			perror("writing data to tmp file");
		else
			fprintf(stderr, "Wrote less bytes than expected, taking"
					"it as an error (%d vs %lu)\n", ret,
					message->size);
		free(file);
		close(fd);
		return NULL;
	}

	close(fd);
	return file;
}

static int
read_message_from_tmpfile(char *file, struct message *message)
{
	int fd, ret;
	assert(file);

	fd = open(file, O_RDONLY);
	if (!fd) {
		perror("Opening tmp file for reading");
		return -1;
	}

	/* XXX do not use hard-coded numbers... */
	/* size of the buffer is 4096 atm */
	ret = read(fd, message->data, 4096);
	if (ret < 0) {
		perror("Reading tmp file\n");
		close(fd);
		return -1;
	}

	assert(ret <= 4096);
	message->size = ret;

	close(fd);
	return 0;
}

static void
destroy_message_tmpfile(char *file)
{
	if (unlink(file) < 0)
		perror("Deleting tmp file");
	free(file);
}

static int
cmd_edit(struct wldbg_interactive *wldbgi,
	 struct message *message,
	 char *buf)
{
	const char *editor;
	char *cmd, *msg_file, edstr[128];
	size_t size = 1024;
	int ret;

	(void) wldbgi;

	if (*buf != '\0') {
		sscanf(buf, "%128s", edstr);
		editor = edstr;
	} else
		editor = getenv("$EDITOR");

	if (!editor) {
		fprintf(stderr, "No edtor to use. Use 'edit editor_name' or "
				"set $EDITOR environment variable\n");
		return CMD_CONTINUE_QUERY;
	}

	msg_file = store_message_to_tmpfile(message);
	if (!msg_file)
		/* the error was written out in the function */
		return CMD_CONTINUE_QUERY;

	do {
		cmd = malloc(sizeof(char) * size);
		if (!cmd) {
			fprintf(stderr, "No memory");
			destroy_message_tmpfile(msg_file);
			return CMD_CONTINUE_QUERY;
		}

		ret = snprintf(cmd, size, "%s %s", editor, msg_file);
		if (ret < 0) {
			perror("Constituting a command");
			destroy_message_tmpfile(msg_file);
			return CMD_CONTINUE_QUERY;
		} else if ((size_t) ret >= size) { /* cmd string too small */
			size *= 2;
			free(cmd);
			cmd = NULL;
		}
	} while (!cmd);

	printf("executing: %s\n", cmd);
	/* XXX maybe use popen? */
	if (system(cmd) != 0) {
		fprintf(stderr, "Executing edit command has returned"
				" non-zero value");
	} else {
		read_message_from_tmpfile(msg_file, message);
	}

	destroy_message_tmpfile(msg_file);
	/* continue or end?? */
	return CMD_CONTINUE_QUERY;
}

static struct print_filter *
create_filter(const char *pattern)
{
	struct print_filter *pf;

	pf = malloc(sizeof *pf);
	if (!pf) {
		fprintf(stderr, "No memory\n");
		return NULL;
	}

	pf->filter = strdup(pattern);
	if (!pf->filter) {
		fprintf(stderr, "No memory\n");
		return NULL;
	}

	return pf;
}

static int
cmd_create_filter(struct wldbg_interactive *wldbgi,
		  char *buf, int show_only)
{
	struct print_filter *pf;
	char filter[128];

	sscanf(buf, "%s", filter);

	pf = create_filter(filter);
	if (!pf)
		return CMD_CONTINUE_QUERY;

	pf->show_only = show_only;
	wl_list_insert(wldbgi->print_filters.next, &pf->link);

	printf("Filtering messages: %s%s\n",
	       show_only ? "" : "hide ", filter);

	return CMD_CONTINUE_QUERY;
}

static int
cmd_hide(struct wldbg_interactive *wldbgi,
	 struct message *message,
	 char *buf)
{
	(void) message;
	return cmd_create_filter(wldbgi, buf, 0);
}

static int
cmd_show_only(struct wldbg_interactive *wldbgi,
	      struct message *message,
	      char *buf)
{
	(void) message;
	return cmd_create_filter(wldbgi, buf, 1);
}

/* XXX keep sorted! (in the future I'd like to do
 * binary search in this array */
const struct command commands[] = {
	{"break", "b", cmd_break, NULL},
	{"continue", "c", cmd_continue, NULL},
	{"edit", "e", cmd_edit, NULL},
	{"help", NULL,  cmd_help, cmd_help_help},
	{"hide", "h",  cmd_hide, NULL},
	{"info", "i", cmd_info, NULL},
	{"next", "n",  cmd_next, NULL},
	{"pass", NULL, cmd_pass, cmd_pass_help},
	{"send", "s", cmd_send, NULL},
	{"showonly", "so", cmd_show_only, NULL},
	{"quit", "q", cmd_quit, NULL},

};

static int
cmd_help(struct wldbg_interactive *wldbgi,
	 struct message *message, char *buf)
{
	size_t i;
	int all = 0;

	(void) wldbgi;
	(void) message;

	if (strcmp(buf, "all\n") == 0)
		all = 1;

	putchar ('\n');

	for (i = 0; i < sizeof commands / sizeof *commands; ++i) {
		if (all)
			printf(" == ");
		else
			putchar('\t');
		
		printf("%s ", commands[i].name);

		if (commands[i].shortcut)
			printf("(%s)", commands[i].shortcut);

		if (all)
			printf(" ==\n\n");

		if (commands[i].help) {
			if (all) {
				commands[i].help(0);
			} else {
				printf("\t -- ");
				commands[i].help(1);
			}	
		}

		if (all)
			putchar('\n');
		putchar('\n');
	}

	return CMD_CONTINUE_QUERY;
}

static char *
next_word(char *str)
{
	int i = 0;

	/* skip the first word if anything left */
	while(isalpha(*(str + i)) && *(str + i) != 0)
		++i;
	/* skip whitespaces before the other word */
	while (isspace(*(str + i)) && *(str + i) != 0)
		++i;

	return str + i;
}

static int
is_the_cmd(char *buf, const struct command *cmd)
{
	int len = 0;

	/* try short version first */
	if (cmd->shortcut)
		len = strlen(cmd->shortcut);

	if (len && strncmp(buf, cmd->shortcut, len) == 0
		&& (isspace(buf[len]) || buf[len] == '\0')) {
		vdbg("identifying command: short '%s' match\n", cmd->shortcut);
		return 1;
	}

	/* this must be set */
	assert(cmd->name && "Each command must have long form");

	len = strlen(cmd->name);
	assert(len);

	if (strncmp(buf, cmd->name, len) == 0
		&& (isspace(buf[len]) || buf[len] == '\0')) {
		vdbg("identifying command: long '%s' match\n", cmd->name);
		return 1;
	}

	vdbg("identifying command: no match\n");
	return 0;
}

int
run_command(char *buf,
		struct wldbg_interactive *wldbgi, struct message *message)
{
	size_t n;

	for (n = 0; n < (sizeof commands / sizeof *commands); ++n) {
		if (is_the_cmd(buf, &commands[n]))
			return commands[n].func(wldbgi, message, next_word(buf));
	}

	return CMD_DONT_MATCH;
}
