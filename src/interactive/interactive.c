/*
 * Copyright (c) 2014 Marek Chalupa
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <assert.h>
#include <sys/signalfd.h>
#include <regex.h>

#include "wldbg.h"
#include "wldbg-pass.h"
#include "wldbg-private.h"
#include "wldbg-parse-message.h"
#include "interactive.h"
#include "resolve.h"
#include "passes.h"
#include "getopt.h"
#include "util.h"

/* defined in interactive-commands.c */
int
cmd_quit(struct wldbg_interactive *wldbgi,
		struct wldbg_message *message, char *buf);

void free_breakpoint(struct breakpoint *);

#define INPUT_BUFFER_SIZE 512
static void
query_user(struct wldbg_interactive *wldbgi, struct wldbg_message *message)
{
	int ret;
	char buf[INPUT_BUFFER_SIZE];
	char *cmd;

	while (1) {
		if (wldbgi->wldbg->flags.exit
			|| wldbgi->wldbg->flags.error) {
			break;
		}

		/* print whatever should have been printed by now
		 * and then print the prompt */
		fflush(stdout);
		printf("(wldbg) ");

		if (fgets(buf, sizeof buf, stdin) == NULL) {
			if(cmd_quit(wldbgi, NULL, NULL) == CMD_END_QUERY)
				break;
			else
				continue;
		}

		cmd = skip_ws_to_newline(buf);

		if (*cmd == '\n' && wldbgi->last_command) {
			cmd = wldbgi->last_command;
		} else if (*cmd == '\n') {
			continue;
		} else {
			/* save last command */
			free(wldbgi->last_command);
			wldbgi->last_command = strdup(cmd);
		}

		ret = run_command(cmd, wldbgi, message);

		if (ret == CMD_END_QUERY)
			break;
		else if (ret == CMD_CONTINUE_QUERY)
			continue;
		else
			printf("Unknown command: %s", cmd);
	}
}

/* return 1 if some of filters matches */
static int
filter_match(struct wl_list *filters, struct wldbg_message *message)
{
	struct filter *pf;
	char buf[128];
	int ret, has_show_only = 0;

	wl_list_for_each(pf, filters, link) {
		ret = wldbg_get_message_name(message, buf, sizeof buf);
		if (ret >= (int) sizeof buf) {
			fprintf(stderr, "BUG: buffer too small for message name\n");
			continue;
		}

		/* run regular expression */
		ret = regexec(&pf->regex, buf, 0, NULL, 0);

		/* got we match? */
		if (ret == 0) {
			vdbg("filter: '%s' <-> '%s' MATCH\n", pf->filter, buf);

			/* If this filter is show_only,
			 * we must return 0, because we'd like to show this message */
			if (pf->show_only)
				return 0;

			return 1;
		} else if (ret != REG_NOMATCH) {
			fprintf(stderr, "Executing regexp failed!\n");
			return 0;
		}

		if (pf->show_only)
			has_show_only = 1;
	}

	/* if we haven't found filter match and we have some show_only filters,
	 * we must return 1 so that this message will get hidden */
	if (has_show_only)
		return 1;

	return 0;
}

static void
process_message(struct wldbg_interactive *wldbgi, struct wldbg_message *message)
{
	/* print message's description
	 * This is default behaviour. XXX add possibility to
	 * turn it off */
	wldbg_message_print(message);

	if (wldbgi->stop) {
		dbg("Stopped at message no. %lu from %s\n",
			message->from == SERVER ?
				wldbgi->statistics.server_msg_no :
				wldbgi->statistics.client_msg_no,
			message->from == SERVER ?
				"server" : "client");
		/* reset flag */
		wldbgi->stop = 0;
		query_user(wldbgi, message);
	}
}

static int
process_interactive(void *user_data, struct wldbg_message *message)
{
	struct wldbg_interactive *wldbgi = user_data;
	struct breakpoint *b;
	int skip_message = 0;

	vdbg("Mesagge from %s\n",
		message->from == SERVER ? "SERVER" : "CLIENT");

	if (message->from == SERVER)
		++wldbgi->statistics.server_msg_no;
	else
		++wldbgi->statistics.client_msg_no;

	if (!wldbgi->skip_first_query
		&& (wldbgi->statistics.server_msg_no
		+ wldbgi->statistics.client_msg_no == 1)) {
		printf("Stopped on the first message\n");
		wldbgi->stop = 1;
	}

	/* if some filter matches, we will skip this message
	 * unless some other condition tell us that we should
	 * not skip it (like breakpoint or so) */
	skip_message = filter_match(&wldbgi->filters, message);

	wl_list_for_each(b, &wldbgi->breakpoints, link) {
		if (b->applies(message, b)) {
			wldbgi->stop = 1;
			/* reset skip_message flag, we want
			 * to stop on this message */
			skip_message = 0;
			break;
		}
	}

	if (!skip_message)
		process_message(wldbgi, message);

	/* This is always the last pass. Even when user will add
	 * some passes interactively, they will be added before
	 * this one */
	return PASS_STOP;
}

/* forward declaration of free_breakpoint,
 * defined in breakpoints.c */
void
free_breakpoint(struct breakpoint *b);

static void
wldbgi_destory(void *data)
{
	struct wldbg_interactive *wldbgi = data;
	struct breakpoint *b, *btmp;
	struct filter *pf, *pftmp;

	dbg("Destroying wldbgi\n");

	wldbgi->wldbg->flags.exit = 1;

	if (wldbgi->client.path)
		free(wldbgi->client.path);

	if (wldbgi->last_command)
		free(wldbgi->last_command);

	wl_list_for_each_safe(b, btmp, &wldbgi->breakpoints, link)
		free_breakpoint(b);
	wl_list_for_each_safe(pf, pftmp, &wldbgi->filters, link) {
		regfree(&pf->regex);
		free(pf->filter);
		free(pf);
	}

	free(wldbgi);
}

static int
handle_sigint(int fd, void *data)
{
	size_t len;
	struct signalfd_siginfo si;
	struct wldbg_interactive *wldbgi = data;

	len = read(fd, &si, sizeof si);
	if (len != sizeof si) {
		fprintf(stderr, "reading signal's fd failed\n");
		return -1;
	}

	vdbg("Wldbgi: Got interrupt (SIGINT)\n");

	putchar('\n');
	query_user(wldbgi, &wldbgi->wldbg->message);

	return 1;
}

int
interactive_init(struct wldbg *wldbg)
{
	struct pass *pass;
	struct wldbg_interactive *wldbgi;
	sigset_t signals;

	dbg("Starting interactive mode.\n");

	wldbgi = malloc(sizeof *wldbgi);
	if (!wldbgi)
		return -1;

	memset(wldbgi, 0, sizeof *wldbgi);
	wl_list_init(&wldbgi->breakpoints);
	wl_list_init(&wldbgi->filters);
	wldbgi->wldbg = wldbg;

	pass = alloc_pass("interactive");
	if (!pass)
		goto err_wldbgi;

	wl_list_insert(wldbg->passes.next, &pass->link);

	pass->wldbg_pass.init = NULL;
	/* XXX ! */
	pass->wldbg_pass.help = NULL;
	pass->wldbg_pass.destroy = wldbgi_destory;
	pass->wldbg_pass.server_pass = process_interactive;
	pass->wldbg_pass.client_pass = process_interactive;
	pass->wldbg_pass.user_data = wldbgi;
	pass->wldbg_pass.description
		= "Interactive pass for wldbg (hardcoded)";
	pass->wldbg_pass.flags = WLDBG_PASS_LOAD_ONCE;

	if (wldbg->flags.pass_whole_buffer) {
		fprintf(stderr, "Interactive mode needs separate messages, "
				"but pass-whole-buffer flag is on.\n");
		goto err_pass;
	}

	/* remove default SIGINT handler */
	sigdelset(&wldbg->handled_signals, SIGINT);
	wldbg->signals_fd = signalfd(wldbg->signals_fd, &wldbg->handled_signals,
					SFD_CLOEXEC);

	if (wldbg->signals_fd == -1)
		goto err_pass;

	sigemptyset(&signals);
	sigaddset(&signals, SIGINT);

	/* set our own signal handlers */
	wldbgi->sigint_fd = signalfd(-1, &signals, SFD_CLOEXEC);

	if (wldbgi->sigint_fd == -1)
		goto err_pass;

	vdbg("Adding interactive SIGINT handler (fd %d)\n", wldbgi->sigint_fd);
	if (wldbg_monitor_fd(wldbg, wldbgi->sigint_fd,
			     handle_sigint, wldbgi) == NULL)
		goto err_pass;

	return 0;

err_pass:
		dealloc_pass(pass);
err_wldbgi:
		free(wldbgi);

		return -1;
}
