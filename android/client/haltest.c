/*
 * Copyright (C) 2013 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <poll.h>
#include <unistd.h>

#include "if-main.h"
#include "terminal.h"
#include "pollhandler.h"
#include "history.h"

const struct interface *interfaces[] = {
	&bluetooth_if,
	&hh_if,
	NULL
};

static struct method commands[];

struct method *get_method(struct method *methods, const char *name)
{
	while (strcmp(methods->name, "") != 0) {
		if (strcmp(methods->name, name) == 0)
			return methods;
		methods++;
	}
	return NULL;
}

/* function returns interface of given name or NULL if not found */
const struct interface *get_interface(const char *name)
{
	int i;

	for (i = 0; interfaces[i] != NULL; ++i) {
		if (strcmp(interfaces[i]->name, name) == 0)
			break;
	}

	return interfaces[i];
}

int haltest_error(const char *format, ...)
{
	va_list args;
	int ret;
	va_start(args, format);
	ret = terminal_vprint(format, args);
	va_end(args);
	return ret;
}

int haltest_info(const char *format, ...)
{
	va_list args;
	int ret;
	va_start(args, format);
	ret = terminal_vprint(format, args);
	va_end(args);
	return ret;
}

int haltest_warn(const char *format, ...)
{
	va_list args;
	int ret;
	va_start(args, format);
	ret = terminal_vprint(format, args);
	va_end(args);
	return ret;
}

static void help_print_interface(const struct interface *i)
{
	struct method *m;

	for (m = i->methods; strcmp(m->name, "") != 0; m++)
		haltest_info("%s %s %s\n", i->name, m->name,
						(m->help ? m->help : ""));
}

/* Help completion */
static void help_c(int argc, const char **argv,
					enum_func *penum_func, void **puser)
{
	if (argc == 2)
		*penum_func = interface_name;
}

/* Help execution */
static void help_p(int argc, const char **argv)
{
	const struct method *m = commands;
	const struct interface **ip = interfaces;
	const struct interface *i;

	if (argc == 1) {
		terminal_print("haltest allows to call Android HAL methods.\n");
		terminal_print("\nAvailable commands:\n");
		while (0 != strcmp(m->name, "")) {
			terminal_print("\t%s %s\n", m->name,
						(m->help ? m->help : ""));
			m++;
		}
		terminal_print("\nAvailable interfaces to use:\n");
		while (NULL != *ip) {
			terminal_print("\t%s\n", (*ip)->name);
			ip++;
		}
		terminal_print("\nTo get help on methods for each interface type:\n");
		terminal_print("\n\thelp <inerface>\n");
		terminal_print("\nBasic scenario:\n\tadapter init\n");
		terminal_print("\tadapter enable\n\tadapter start_discovery\n");
		terminal_print("\tadapter get_profile_interface handsfree\n");
		terminal_print("\thandsfree init\n\n");
		return;
	}
	i = get_interface(argv[1]);
	if (i == NULL) {
		haltest_error("No such interface\n");
		return;
	}
	help_print_interface(i);
}

/* quit/exit execution */
static void quit_p(int argc, const char **argv)
{
	exit(0);
}

/* Commands available without interface */
static struct method commands[] = {
	STD_METHODCH(help, "[<interface>]"),
	STD_METHOD(quit),
	METHOD("exit", quit_p, NULL, NULL),
	END_METHOD
};

/* Gets comman by name */
struct method *get_command(const char *name)
{
	return get_method(commands, name);
}

/* Function to enumerate interface names */
const char *interface_name(void *v, int i)
{
	return interfaces[i] ? interfaces[i]->name : NULL;
}

/* Function to enumerate command and interface names */
const char *command_name(void *v, int i)
{
	int cmd_cnt = (int) (sizeof(commands)/sizeof(commands[0]) - 1);
	if (i >= cmd_cnt)
		return interface_name(v, i - cmd_cnt);
	else
		return commands[i].name;
}

/*
 * This function changes input parameter line_buffer so it has
 * null termination after each token (due to strtok)
 * Output argv is filled with pointers to arguments
 * returns number of tokens parsed - argc
 */
static int command_line_to_argv(char *line_buffer,
				char *argv[], int argv_size)
{
	static const char *token_breaks = "\r\n\t ";
	char *token;
	int argc = 0;

	token = strtok(line_buffer, token_breaks);
	while (token != NULL && argc < (int) argv_size) {
		argv[argc++] = token;
		token = strtok(NULL, token_breaks);
	}

	return argc;
}

static void process_line(char *line_buffer)
{
	char *argv[10];
	int argc;
	int i = 0;
	struct method *m;

	argc = command_line_to_argv(line_buffer, argv, 10);
	if (argc < 1)
		return;

	while (interfaces[i] != NULL) {
		if (strcmp(interfaces[i]->name, argv[0])) {
			i++;
			continue;
		}
		if (argc < 2 || strcmp(argv[1], "?") == 0) {
			help_print_interface(interfaces[i]);
			return;
		}
		m = get_method(interfaces[i]->methods, argv[1]);
		if (m != NULL) {
			m->func(argc, (const char **) argv);
			return;
		}
		haltest_error("No function %s found\n", argv[1]);
		return;
	}
	/* No interface, try commands */
	m = get_command(argv[0]);
	if (m == NULL)
		haltest_error("No such command %s\n", argv[0]);
	else
		m->func(argc, (const char **) argv);
}

/* called when there is something on stdin */
static void stdin_handler(struct pollfd *pollfd)
{
	char buf[10];

	if (pollfd->revents & POLLIN) {
		int count = read(0, buf, 10);

		if (count > 0) {
			int i;

			for (i = 0; i < count; ++i)
				terminal_process_char(buf[i], process_line);
		}
	}
}

int main(int argc, char **argv)
{
	terminal_setup();
	history_restore(".haltest_history");

	/* Register command line handler */
	poll_register_fd(0, POLLIN, stdin_handler);

	poll_dispatch_loop();

	return 0;
}