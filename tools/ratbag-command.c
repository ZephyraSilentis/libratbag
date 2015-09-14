/*
 * Copyright © 2015 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <linux/input.h>

#include "shared.h"

enum options {
	OPT_VERBOSE,
	OPT_HELP,
};

enum errors {
	SUCCESS = 0,
	ERR_UNSUPPORTED = 1,	/* device doesn't support function, or
				   an index exceeds the device */
	ERR_USAGE = 2,		/* invalid commandline */
	ERR_DEVICE = 3,		/* invalid/missing device or command failed */
};

enum cmd_flags {
	FLAG_VERBOSE = 1 << 0,
	FLAG_VERBOSE_RAW = 1 << 1,

	/* flags used in ratbag_cmd */
	FLAG_NEED_DEVICE = 1 << 10,
	FLAG_NEED_PROFILE = 1 << 11,
	FLAG_NEED_RESOLUTION = 1 << 12,
};

struct ratbag_cmd_options {
	enum cmd_flags flags;
	struct ratbag_device *device;
	struct ratbag_profile *profile;
	struct ratbag_resolution *resolution;
	int button;
};

struct ratbag_cmd {
	const char *name;
	int (*cmd)(const struct ratbag_cmd *cmd,
		   struct ratbag *ratbag,
		   struct ratbag_cmd_options *options,
		   int argc, char **argv);
	const char *args;
	const char *help;
	uint32_t flags;
	const struct ratbag_cmd *subcommands[];
};

static const struct ratbag_cmd *ratbag_commands;

static void
usage_subcommand(const struct ratbag_cmd *cmd, const char *prefix_in)
{
	int i = 0;
	int count;
	const struct ratbag_cmd *sub;
	char prefix[256];

	if (cmd->subcommands[0] == NULL)
		return;

	sub = cmd->subcommands[0];
	while (sub) {
		count = 40 - strlen(sub->name);
		if (sub->args)
			count -= 1 + strlen(sub->args);
		if (count < 4)
			count = 4;

		sprintf(prefix, "%s%s%s%s ",
			prefix_in,
			cmd->name,
			cmd->args ?  " " : "",
			cmd->args ?  cmd->args : "");
		count -= strlen(prefix);
		if (sub->help)
			printf("    %s%s%s%s %.*s %s\n",
			       prefix,
			       sub->name,
			       sub->args ? " " : "",
			       sub->args ? sub->args : "",
			       count,
			       ".........................................",
			       sub->help);

		usage_subcommand(sub, prefix);

		sub = cmd->subcommands[++i];
	}
}

static void
usage(void)
{
	printf("Usage: %s [options] [command] /sys/class/input/eventX\n"
	       "/path/to/device ..... Open the given device only\n"
	       "\n"
	       "Commands:\n",
		program_invocation_short_name);


	usage_subcommand(ratbag_commands, "");

	printf("\n"
	       "Options:\n"
	       "    --verbose[=raw] ....... Print debugging output, with protocol output if requested.\n"
	       "    --help .......... Print this help.\n");
}

static inline struct ratbag_device *
ratbag_cmd_device_from_arg(struct ratbag *ratbag,
			   int *argc, char **argv)
{
	struct ratbag_device *device;
	const char *path;

	if (*argc == 0) {
		error("Missing device path.\n");
		usage();
		return NULL;
	}

	path = argv[*argc - 1];
	device = ratbag_cmd_open_device(ratbag, path);
	if (!device) {
		error("Device '%s' is not supported\n", path);
		return NULL;
	}

	(*argc)--;

	return device;
}

static inline struct ratbag_profile *
ratbag_cmd_get_active_profile(struct ratbag_device *device)
{
	struct ratbag_profile *profile = NULL;
	int i;

	for (i = 0; i < ratbag_device_get_num_profiles(device); i++) {
		profile = ratbag_device_get_profile_by_index(device, i);
		if (ratbag_profile_is_active(profile))
			return profile;

		ratbag_profile_unref(profile);
		profile = NULL;
	}

	if (!profile)
		error("Failed to retrieve the active profile\n");

	return NULL;
}

static inline struct ratbag_resolution *
ratbag_cmd_get_active_resolution(struct ratbag_profile *profile)
{
	struct ratbag_resolution *resolution = NULL;
	int i;

	for (i = 0; i < ratbag_profile_get_num_resolutions(profile); i++) {
		resolution = ratbag_profile_get_resolution(profile, i);
		if (ratbag_resolution_is_active(resolution))
			return resolution;

		ratbag_resolution_unref(resolution);
		resolution = NULL;
	}

	if (!resolution)
		error("Failed to retrieve the active resolution\n");

	return NULL;
}

static inline int
fill_options(struct ratbag *ratbag,
	     struct ratbag_cmd_options *options,
	     uint32_t flags,
	     int *argc, char **argv)
{
	struct ratbag_device *device = options->device;
	struct ratbag_profile *profile = options->profile;
	struct ratbag_resolution *resolution = options->resolution;

	if ((flags & (FLAG_NEED_DEVICE|FLAG_NEED_PROFILE|FLAG_NEED_RESOLUTION)) &&
	    device == NULL) {
		device = ratbag_cmd_device_from_arg(ratbag, argc, argv);
		if (!device)
			return ERR_DEVICE;
		options->device = device;
	}

	if ((flags & (FLAG_NEED_PROFILE|FLAG_NEED_RESOLUTION)) &&
	     profile == NULL) {
		profile = ratbag_cmd_get_active_profile(device);
		if (!profile)
			return ERR_DEVICE;
		options->profile = profile;
	}

	if (flags & FLAG_NEED_RESOLUTION && resolution == NULL) {
		resolution = ratbag_cmd_get_active_resolution(profile);
		if (!resolution)
			return ERR_DEVICE;
		options->resolution = resolution;
	}

	return SUCCESS;
}

static int
run_subcommand(const char *command,
	       const struct ratbag_cmd *cmd,
	       struct ratbag *ratbag,
	       struct ratbag_cmd_options *options,
	       int argc, char **argv)
{
	const struct ratbag_cmd *sub = cmd->subcommands[0];
	int i = 0;
	int rc;

	while (sub) {
		if (streq(command, sub->name)) {
			rc = fill_options(ratbag, options,
					  sub->flags,
					  &argc,
					  argv);
			if (rc != SUCCESS)
				return rc;

			argc--;
			argv++;
			return sub->cmd(sub, ratbag, options, argc, argv);
		}
		sub = cmd->subcommands[i++];
	}

	error("Invalid subcommand '%s'\n", command);
	return ERR_USAGE;
}

static int
ratbag_cmd_info(const struct ratbag_cmd *cmd,
		struct ratbag *ratbag,
		struct ratbag_cmd_options *options,
		int argc, char **argv)
{
	struct ratbag_device *device;
	struct ratbag_profile *profile;
	struct ratbag_button *button;
	char *action;
	int num_profiles, num_buttons;
	int i, j, b;

	device = options->device;

	printf("Device '%s'\n", ratbag_device_get_name(device));

	printf("Capabilities:");
	if (ratbag_device_has_capability(device,
					 RATBAG_DEVICE_CAP_SWITCHABLE_RESOLUTION))
		printf(" res");
	if (ratbag_device_has_capability(device,
					 RATBAG_DEVICE_CAP_SWITCHABLE_PROFILE))
		printf(" profile");
	if (ratbag_device_has_capability(device,
					 RATBAG_DEVICE_CAP_BUTTON_KEY))
		printf(" btn-key");
	if (ratbag_device_has_capability(device,
					 RATBAG_DEVICE_CAP_BUTTON_MACROS))
		printf(" btn-macros");
	printf("\n");

	num_buttons = ratbag_device_get_num_buttons(device);
	printf("Number of buttons: %d\n", num_buttons);

	num_profiles = ratbag_device_get_num_profiles(device);
	printf("Profiles supported: %d\n", num_profiles);

	for (i = 0; i < num_profiles; i++) {
		int dpi, rate;
		profile = ratbag_device_get_profile_by_index(device, i);
		if (!profile)
			continue;

		printf("  Profile %d%s%s\n", i,
		       ratbag_profile_is_active(profile) ? " (active)" : "",
		       ratbag_profile_is_default(profile) ? " (default)" : "");
		printf("    Resolutions:\n");
		for (j = 0; j < ratbag_profile_get_num_resolutions(profile); j++) {
			struct ratbag_resolution *res;

			res = ratbag_profile_get_resolution(profile, j);
			dpi = ratbag_resolution_get_dpi(res);
			rate = ratbag_resolution_get_report_rate(res);
			if (dpi == 0)
				printf("      %d: <disabled>\n", j);
			else if (ratbag_resolution_has_capability(res,
								  RATBAG_RESOLUTION_CAP_SEPARATE_XY_RESOLUTION))
				printf("      %d: %dx%ddpi @ %dHz%s%s\n", j,
				       ratbag_resolution_get_dpi_x(res),
				       ratbag_resolution_get_dpi_y(res),
				       rate,
				       ratbag_resolution_is_active(res) ? " (active)" : "",
				       ratbag_resolution_is_default(res) ? " (default)" : "");
			else
				printf("      %d: %ddpi @ %dHz%s%s\n", j, dpi, rate,
				       ratbag_resolution_is_active(res) ? " (active)" : "",
				       ratbag_resolution_is_default(res) ? " (default)" : "");

			ratbag_resolution_unref(res);
		}

		for (b = 0; b < num_buttons; b++) {
			enum ratbag_button_type type;

			button = ratbag_profile_get_button_by_index(profile, b);
			type = ratbag_button_get_type(button);
			action = button_action_to_str(button);
			printf("    Button: %d type %s is mapped to '%s'\n",
			       b, button_type_to_str(type), action);
			free(action);
			button = ratbag_button_unref(button);
		}

		profile = ratbag_profile_unref(profile);
	}

	return SUCCESS;
}

static const struct ratbag_cmd cmd_info = {
	.name = "info",
	.cmd = ratbag_cmd_info,
	.args = NULL,
	.help = "Show information about the device's capabilities",
	.flags = FLAG_NEED_DEVICE,
	.subcommands = { NULL },
};

static int
ratbag_cmd_switch_etekcity(const struct ratbag_cmd *cmd,
			   struct ratbag *ratbag,
			   struct ratbag_cmd_options *options,
			   int argc, char **argv)
{
	struct ratbag_device *device;
	struct ratbag_button *button_6, *button_7;
	struct ratbag_profile *profile = NULL;
	int commit = 0;
	unsigned int modifiers[10];
	size_t modifiers_sz = 10;

	device = options->device;
	profile = options->profile;

	if (!ratbag_device_has_capability(device,
					  RATBAG_DEVICE_CAP_SWITCHABLE_PROFILE)) {
		error("Device '%s' has no switchable profiles\n",
		      ratbag_device_get_name(device));
		return ERR_UNSUPPORTED;
	}

	button_6 = ratbag_profile_get_button_by_index(profile, 6);
	button_7 = ratbag_profile_get_button_by_index(profile, 7);

	if (ratbag_button_get_key(button_6, modifiers, &modifiers_sz) == KEY_VOLUMEUP &&
	    ratbag_button_get_key(button_7, modifiers, &modifiers_sz) == KEY_VOLUMEDOWN) {
		ratbag_button_disable(button_6);
		ratbag_button_disable(button_7);
		commit = 1;
	} else if (ratbag_button_get_action_type(button_6) == RATBAG_BUTTON_ACTION_TYPE_NONE &&
		   ratbag_button_get_action_type(button_7) == RATBAG_BUTTON_ACTION_TYPE_NONE) {
		ratbag_button_set_key(button_6, KEY_VOLUMEUP, modifiers, 0);
		ratbag_button_set_key(button_7, KEY_VOLUMEDOWN, modifiers, 0);
		commit = 2;
	}

	button_6 = ratbag_button_unref(button_6);
	button_7 = ratbag_button_unref(button_7);

	printf("Switched the current profile of '%s' to %sreport the volume keys\n",
	       ratbag_device_get_name(device),
	       commit == 1 ? "not " : "");

	return SUCCESS;
}

static const struct ratbag_cmd cmd_switch_etekcity = {
	.name = "switch-etekcity",
	.cmd = ratbag_cmd_switch_etekcity,
	.args = NULL,
	.help = "Switch the Etekcity mouse active profile",
	.flags = FLAG_NEED_DEVICE | FLAG_NEED_PROFILE,
	.subcommands = { NULL },
};

struct macro {
	const char *name;
	struct {
		enum ratbag_macro_event_type type;
		unsigned data;
	} events[64];
};

static int
str_to_macro(const char *action_arg, struct macro *m)
{
	if (!action_arg)
		return -EINVAL;

	if (action_arg[0] == 'f') {
		m->name = "foo";
		m->events[0].type = RATBAG_MACRO_EVENT_KEY_PRESSED;
		m->events[0].data = KEY_F;
		m->events[1].type = RATBAG_MACRO_EVENT_KEY_RELEASED;
		m->events[1].data = KEY_F;
		m->events[2].type = RATBAG_MACRO_EVENT_KEY_PRESSED;
		m->events[2].data = KEY_O;
		m->events[3].type = RATBAG_MACRO_EVENT_KEY_RELEASED;
		m->events[3].data = KEY_O;
		m->events[4].type = RATBAG_MACRO_EVENT_KEY_PRESSED;
		m->events[4].data = KEY_O;
		m->events[5].type = RATBAG_MACRO_EVENT_KEY_RELEASED;
		m->events[5].data = KEY_O;
	} else if (action_arg[0] == 'b') {
		m->name = "bar";
		m->events[0].type = RATBAG_MACRO_EVENT_KEY_PRESSED;
		m->events[0].data = KEY_B;
		m->events[1].type = RATBAG_MACRO_EVENT_KEY_RELEASED;
		m->events[1].data = KEY_B;
		m->events[2].type = RATBAG_MACRO_EVENT_KEY_PRESSED;
		m->events[2].data = KEY_A;
		m->events[3].type = RATBAG_MACRO_EVENT_KEY_RELEASED;
		m->events[3].data = KEY_A;
		m->events[4].type = RATBAG_MACRO_EVENT_KEY_PRESSED;
		m->events[4].data = KEY_R;
		m->events[5].type = RATBAG_MACRO_EVENT_KEY_RELEASED;
		m->events[5].data = KEY_R;
	}

	return 0;
}

static int
ratbag_cmd_change_button(const struct ratbag_cmd *cmd,
			 struct ratbag *ratbag,
			 struct ratbag_cmd_options *options,
			 int argc, char **argv)
{
	const char *action_str, *action_arg;
	struct ratbag_device *device;
	struct ratbag_button *button = NULL;
	struct ratbag_profile *profile = NULL;
	int button_index;
	enum ratbag_button_action_type action_type;
	int rc = ERR_DEVICE;
	unsigned int btnkey;
	enum ratbag_button_action_special special;
	struct macro macro = {0};
	int i;

	if (argc != 3)
		return ERR_USAGE;

	button_index = atoi(argv[0]);
	action_str = argv[1];
	action_arg = argv[2];

	argc -= 3;
	argv += 3;

	if (streq(action_str, "button")) {
		action_type = RATBAG_BUTTON_ACTION_TYPE_BUTTON;
		btnkey = atoi(action_arg);
	} else if (streq(action_str, "key")) {
		action_type = RATBAG_BUTTON_ACTION_TYPE_KEY;
		btnkey = libevdev_event_code_from_name(EV_KEY, action_arg);
		if (!btnkey) {
			error("Failed to resolve key %s\n", action_arg);
			return ERR_USAGE;
		}
	} else if (streq(action_str, "special")) {
		action_type = RATBAG_BUTTON_ACTION_TYPE_SPECIAL;
		special = str_to_special_action(action_arg);
		if (special == RATBAG_BUTTON_ACTION_SPECIAL_INVALID) {
			error("Invalid special command '%s'\n", action_arg);
			return ERR_USAGE;
		}
	} else if (streq(action_str, "macro")) {
		action_type = RATBAG_BUTTON_ACTION_TYPE_MACRO;
		if (str_to_macro(action_arg, &macro)) {
			error("Invalid special command '%s'\n", action_arg);
			return ERR_USAGE;
		}
	} else {
		return ERR_USAGE;
	}

	device = options->device;
	profile = options->profile;

	if (!ratbag_device_has_capability(device,
					  RATBAG_DEVICE_CAP_BUTTON_KEY)) {
		error("Device '%s' has no programmable buttons\n",
		      ratbag_device_get_name(device));
		rc = ERR_UNSUPPORTED;
		goto out;
	}

	button = ratbag_profile_get_button_by_index(profile, button_index);
	if (!button) {
		error("Invalid button number %d\n", button_index);
		rc = ERR_UNSUPPORTED;
		goto out;
	}

	switch (action_type) {
	case RATBAG_BUTTON_ACTION_TYPE_BUTTON:
		rc = ratbag_button_set_button(button, btnkey);
		break;
	case RATBAG_BUTTON_ACTION_TYPE_KEY:
		rc = ratbag_button_set_key(button, btnkey, NULL, 0);
		break;
	case RATBAG_BUTTON_ACTION_TYPE_SPECIAL:
		rc = ratbag_button_set_special(button, special);
		break;
	case RATBAG_BUTTON_ACTION_TYPE_MACRO:
		rc = ratbag_button_set_macro(button, macro.name);
		for (i = 0; i < ARRAY_LENGTH(macro.events); i++) {
			if (macro.events[i].type == RATBAG_MACRO_EVENT_NONE)
				break;

			ratbag_button_set_macro_event(button,
						      i,
						      macro.events[i].type,
						      macro.events[i].data);
		}
		rc = ratbag_button_write_macro(button);
		break;
	default:
		error("well, that shouldn't have happened\n");
		abort();
		break;
	}
	if (rc) {
		error("Unable to perform button %d mapping %s %s\n",
		      button_index,
		      action_str,
		      action_arg);
		rc = ERR_UNSUPPORTED;
		goto out;
	}

	rc = ratbag_profile_set_active(profile);
	if (rc) {
		error("Unable to apply the current profile: %s (%d)\n",
		      strerror(-rc),
		      rc);
		rc = ERR_DEVICE;
		goto out;
	}

out:
	button = ratbag_button_unref(button);

	return rc;
}

static const struct ratbag_cmd cmd_change_button = {
	.name = "change-button",
	.cmd = ratbag_cmd_change_button,
	.args = "X <button|key|special|macro> <number|KEY_FOO|special|macro name:KEY_FOO,KEY_BAR,...>",
	.help = "Remap button X to the given action in the active profile",
	.flags = FLAG_NEED_DEVICE | FLAG_NEED_PROFILE,
	.subcommands = { NULL },
};

static int
filter_event_node(const struct dirent *input_entry)
{
	return strneq(input_entry->d_name, "event", 5);
}

static int
ratbag_cmd_list_supported_devices(const struct ratbag_cmd *cmd,
				  struct ratbag *ratbag,
				  struct ratbag_cmd_options *options,
				  int argc, char **argv)
{
	struct dirent **input_list;
	struct ratbag_device *device;
	char path[256];
	int n, i;
	int supported = 0;

	if (argc != 0)
		return ERR_USAGE;

	n = scandir("/dev/input", &input_list, filter_event_node, alphasort);
	if (n < 0)
		return SUCCESS;

	i = -1;
	while (++i < n) {
		sprintf(path, "/dev/input/%s", input_list[i]->d_name);
		device = ratbag_cmd_open_device(ratbag, path);
		if (device) {
			printf("%s:\t%s\n", path, ratbag_device_get_name(device));
			device = ratbag_device_unref(device);
			supported++;
		}
		free(input_list[i]);
	}
	free(input_list);

	if (!supported)
		printf("No supported devices found\n");

	return SUCCESS;
}

static const struct ratbag_cmd cmd_list = {
	.name = "list",
	.cmd = ratbag_cmd_list_supported_devices,
	.args = NULL,
	.help = "List the available devices",
	.flags = 0,
	.subcommands = { NULL },
};

static int
ratbag_cmd_resolution_active_set(const struct ratbag_cmd *cmd,
				 struct ratbag *ratbag,
				 struct ratbag_cmd_options *options,
				 int argc, char **argv)
{

	printf("Not yet implemented\n");

	return SUCCESS;
}

static const struct ratbag_cmd cmd_resolution_active_set = {
	.name = "set",
	.cmd = ratbag_cmd_resolution_active_set,
	.args = "M",
	.help = "Set the active resolution number",
	.flags = FLAG_NEED_DEVICE | FLAG_NEED_PROFILE,
	.subcommands = { NULL },
};

static int
ratbag_cmd_resolution_active_get(const struct ratbag_cmd *cmd,
				 struct ratbag *ratbag,
				 struct ratbag_cmd_options *options,
				 int argc, char **argv)
{
	printf("Not yet implemented\n");

	return SUCCESS;
}

static const struct ratbag_cmd cmd_resolution_active_get = {
	.name = "get",
	.cmd = ratbag_cmd_resolution_active_get,
	.args = NULL,
	.help = "Get the active resolution number",
	.flags = FLAG_NEED_DEVICE | FLAG_NEED_PROFILE,
	.subcommands = { NULL },
};

static int
ratbag_cmd_resolution_active(const struct ratbag_cmd *cmd,
			  struct ratbag *ratbag,
			  struct ratbag_cmd_options *options,
			  int argc, char **argv)
{
	if (argc < 1)
		return ERR_USAGE;

	return run_subcommand(argv[0],
			      cmd,
			      ratbag, options,
			      argc, argv);
}

static const struct ratbag_cmd cmd_resolution_active = {
	.name = "active",
	.cmd = ratbag_cmd_resolution_active,
	.args = NULL,
	.help = NULL,
	.flags = FLAG_NEED_DEVICE | FLAG_NEED_PROFILE,
	.subcommands = {
		&cmd_resolution_active_get,
		&cmd_resolution_active_set,
		NULL,
	},
};

static int
ratbag_cmd_resolution_dpi_get(const struct ratbag_cmd *cmd,
			      struct ratbag *ratbag,
			      struct ratbag_cmd_options *options,
			      int argc, char **argv)
{
	struct ratbag_resolution *resolution;
	int dpi;

	resolution = options->resolution;
	dpi = ratbag_resolution_get_dpi(resolution);
	printf("%d\n", dpi);

	return SUCCESS;
}

static const struct ratbag_cmd cmd_resolution_dpi_get = {
	.name = "get",
	.cmd = ratbag_cmd_resolution_dpi_get,
	.args = NULL,
	.help = "Get the resolution in dpi",
	.flags = FLAG_NEED_DEVICE | FLAG_NEED_PROFILE | FLAG_NEED_RESOLUTION,
	.subcommands = { NULL },
};

static int
ratbag_cmd_resolution_dpi_set(const struct ratbag_cmd *cmd,
			      struct ratbag *ratbag,
			      struct ratbag_cmd_options *options,
			      int argc, char **argv)
{
	struct ratbag_device *device;
	struct ratbag_resolution *resolution;
	int rc = SUCCESS;
	int dpi;

	if (argc != 1)
		return ERR_USAGE;

	dpi = atoi(argv[0]);

	argc--;
	argv++;

	device = options->device;
	resolution = options->resolution;

	if (!ratbag_device_has_capability(device,
					  RATBAG_DEVICE_CAP_SWITCHABLE_RESOLUTION)) {
		error("Device '%s' has no switchable resolution\n",
		      ratbag_device_get_name(device));
		rc = ERR_UNSUPPORTED;
		goto out;
	}

	rc = ratbag_resolution_set_dpi(resolution, dpi);
	if (rc) {
		error("Failed to change the dpi: %s (%d)\n",
		      strerror(-rc),
		      rc);
		rc = ERR_DEVICE;
	}
out:
	return rc;
}

static const struct ratbag_cmd cmd_resolution_dpi_set = {
	.name = "set",
	.cmd = ratbag_cmd_resolution_dpi_set,
	.args = "<dpi>",
	.help = "Set the resolution in dpi",
	.flags = FLAG_NEED_DEVICE | FLAG_NEED_PROFILE| FLAG_NEED_RESOLUTION,
	.subcommands = { NULL },
};

static int
ratbag_cmd_resolution_dpi(const struct ratbag_cmd *cmd,
			  struct ratbag *ratbag,
			  struct ratbag_cmd_options *options,
			  int argc, char **argv)
{
	if (argc < 1)
		return ERR_USAGE;

	return run_subcommand(argv[0],
			      cmd,
			      ratbag, options,
			      argc, argv);
}

static const struct ratbag_cmd cmd_resolution_dpi = {
	.name = "dpi",
	.cmd = ratbag_cmd_resolution_dpi,
	.args = NULL,
	.help = NULL,
	.flags = FLAG_NEED_DEVICE | FLAG_NEED_PROFILE| FLAG_NEED_RESOLUTION,
	.subcommands = {
		&cmd_resolution_dpi_get,
		&cmd_resolution_dpi_set,
		NULL,
	},
};

static int
ratbag_cmd_resolution(const struct ratbag_cmd *cmd,
		      struct ratbag *ratbag,
		      struct ratbag_cmd_options *options,
		      int argc, char **argv)
{
	struct ratbag_profile *profile;
	struct ratbag_resolution *resolution;
	const char *command;
	int resolution_idx = 0;
	char *endp;

	if (argc < 1)
		return ERR_USAGE;

	command = argv[0];

	profile = options->profile;

	resolution_idx = strtod(command, &endp);
	if (command != endp && *endp == '\0') {
		resolution = ratbag_profile_get_resolution(profile,
							   resolution_idx);

		if (!resolution) {
			error("Unable to retrieve resolution %d\n",
			      resolution_idx);
			return ERR_UNSUPPORTED;
		}
		argc--;
		argv++;
		command = argv[0];
	} else {
		resolution = ratbag_cmd_get_active_resolution(profile);
		if (!resolution)
			return ERR_DEVICE;
	}

	options->resolution = resolution;

	return run_subcommand(command,
			      cmd,
			      ratbag, options,
			      argc, argv);
}

static const struct ratbag_cmd cmd_resolution = {
	.name = "resolution",
	.cmd = ratbag_cmd_resolution,
	.args = "N",
	.help = NULL,
	.flags = FLAG_NEED_DEVICE | FLAG_NEED_PROFILE,
	.subcommands = {
		&cmd_resolution_active,
		&cmd_resolution_dpi,
		NULL,
	},
};

static int
ratbag_cmd_button(const struct ratbag_cmd *cmd,
		   struct ratbag *ratbag,
		   struct ratbag_cmd_options *options,
		   int argc, char **argv)
{
	const char *command;
	int button = 0;
	char *endp;

	if (argc < 2)
		return ERR_USAGE;

	command = argv[1];

	button = strtod(command, &endp);
	if (command != endp && *endp == '\0') {
		options->button = button;
		argc--;
		argv++;
	}

	return run_subcommand(command,
			      cmd,
			      ratbag, options,
			      argc, argv);
}

static const struct ratbag_cmd cmd_button = {
	.name = "button",
	.cmd = ratbag_cmd_button,
	.args = "[...]",
	.help = "Modify a button",
	.flags = FLAG_NEED_DEVICE | FLAG_NEED_PROFILE,
	.subcommands = {
		/* FIXME */
		NULL,
	},
};

static int
ratbag_cmd_profile_active_set(const struct ratbag_cmd *cmd,
			      struct ratbag *ratbag,
			      struct ratbag_cmd_options *options,
			      int argc, char **argv)
{
	struct ratbag_device *device;
	struct ratbag_profile *profile = NULL, *active_profile = NULL;
	int num_profiles, index;
	int rc = ERR_UNSUPPORTED;

	if (argc != 1)
		return ERR_USAGE;

	index = atoi(argv[0]);

	argc--;
	argv++;

	device = options->device;

	if (!device)
		return ERR_DEVICE;

	if (!ratbag_device_has_capability(device,
					  RATBAG_DEVICE_CAP_SWITCHABLE_PROFILE)) {
		error("Device '%s' has no switchable profiles\n",
		      ratbag_device_get_name(device));
		goto out;
	}

	num_profiles = ratbag_device_get_num_profiles(device);
	if (index > num_profiles) {
		error("'%d' is not a valid profile\n", index);
		goto out;
	}

	profile = ratbag_device_get_profile_by_index(device, index);
	if (ratbag_profile_is_active(profile)) {
		printf("'%s' is already in profile '%d'\n",
		       ratbag_device_get_name(device), index);
		rc = SUCCESS;
		goto out;
	}

	rc = ratbag_profile_set_active(profile);
	if (rc == 0) {
		printf("Switched '%s' to profile '%d'\n",
		       ratbag_device_get_name(device), index);
		rc = SUCCESS;
	} else {
		rc = ERR_DEVICE;
	}

out:
	profile = ratbag_profile_unref(profile);
	active_profile = ratbag_profile_unref(active_profile);

	return rc;
}

static const struct ratbag_cmd cmd_profile_active_set = {
	.name = "set",
	.cmd = ratbag_cmd_profile_active_set,
	.args = "N",
	.help = "Set the active profile number",
	.flags = FLAG_NEED_DEVICE,
	.subcommands = { NULL },
};

static int
ratbag_cmd_profile_active_get(const struct ratbag_cmd *cmd,
			      struct ratbag *ratbag,
			      struct ratbag_cmd_options *options,
			      int argc, char **argv)
{
	struct ratbag_device *device;
	struct ratbag_profile *profile = NULL;
	int i;
	int rc = ERR_DEVICE;
	int active_profile_idx = 0;
	int num_profiles = 0;

	device = options->device;

	if (!ratbag_device_has_capability(device,
					  RATBAG_DEVICE_CAP_SWITCHABLE_PROFILE)) {
		rc = SUCCESS;
		goto out;
	}

	num_profiles = ratbag_device_get_num_profiles(device);
	if (num_profiles <= 1) {
		rc = SUCCESS;
		goto out;
	}

	for (i = 0; i < num_profiles; i++) {
		profile = ratbag_device_get_profile_by_index(device, i);
		if (ratbag_profile_is_active(profile)) {
			active_profile_idx = i;
			rc = SUCCESS;
			break;
		}
		ratbag_profile_unref(profile);
		profile = NULL;
	}

	if (active_profile_idx >= num_profiles)
		error("Unable to find active profile, this is a bug.\n");

out:
	if (rc == SUCCESS)
		printf("%d\n", active_profile_idx);
	profile = ratbag_profile_unref(profile);
	return rc;
}

static const struct ratbag_cmd cmd_profile_active_get = {
	.name = "get",
	.cmd = ratbag_cmd_profile_active_get,
	.args = NULL,
	.help = "Get the active profile number",
	.flags = FLAG_NEED_DEVICE,
	.subcommands = { NULL },
};

static int
ratbag_cmd_profile_active(const struct ratbag_cmd *cmd,
			  struct ratbag *ratbag,
			  struct ratbag_cmd_options *options,
			  int argc, char **argv)
{
	if (argc < 1)
		return ERR_USAGE;

	return run_subcommand(argv[0],
			      cmd,
			      ratbag, options,
			      argc, argv);
}

static const struct ratbag_cmd cmd_profile_active = {
	.name = "active",
	.cmd = ratbag_cmd_profile_active,
	.args = NULL,
	.help = NULL,
	.flags = FLAG_NEED_DEVICE,
	.subcommands = {
		&cmd_profile_active_get,
		&cmd_profile_active_set,
		NULL,
	},
};

static int
ratbag_cmd_profile(const struct ratbag_cmd *cmd,
		   struct ratbag *ratbag,
		   struct ratbag_cmd_options *options,
		   int argc, char **argv)
{
	struct ratbag_profile *profile;
	struct ratbag_device *device;
	const char *command;
	int profile_idx = 0;
	char *endp;

	device = options->device;

	if (argc < 1)
		return ERR_USAGE;

	command = argv[0];

	profile_idx = strtod(command, &endp);
	if (command != endp && *endp == '\0') {
		profile = ratbag_device_get_profile_by_index(device,
							     profile_idx);
		if (!profile) {
			error("Unable to find profile %d\n", profile_idx);
			return ERR_UNSUPPORTED;
		}

		argc--;
		argv++;
		command = argv[0];
	} else {
		profile = ratbag_cmd_get_active_profile(device);
		if (!profile)
			return ERR_DEVICE;
	}

	options->profile = profile;

	return run_subcommand(command,
			      cmd,
			      ratbag, options,
			      argc, argv);
}

static const struct ratbag_cmd cmd_profile = {
	.name = "profile",
	.cmd = ratbag_cmd_profile,
	.args = "<idx>",
	.help = NULL,
	.flags = FLAG_NEED_DEVICE,
	.subcommands = {
		&cmd_profile_active,
		&cmd_resolution,
		&cmd_button,
		NULL,
	},
};

static const struct ratbag_cmd top_level_commands = {
	.name = "ratbag-command",
	.cmd = NULL,
	.args = NULL,
	.help = NULL,
	.subcommands = {
		&cmd_info,
		&cmd_list,
		&cmd_change_button,
		&cmd_switch_etekcity,
		&cmd_button,
		&cmd_resolution,
		&cmd_profile,
		&cmd_resolution_dpi,
		NULL,
	},
};

static const struct ratbag_cmd *ratbag_commands = &top_level_commands;

int
main(int argc, char **argv)
{
	struct ratbag *ratbag;
	const char *command;
	int rc = SUCCESS;
	struct ratbag_cmd_options options = {0};

	ratbag = ratbag_create_context(&interface, NULL);
	if (!ratbag) {
		rc = ERR_DEVICE;
		error("Failed to initialize ratbag\n");
		goto out;
	}

	options.flags = 0;
	options.button = -1;

	while (1) {
		int c;
		int option_index = 0;
		static struct option opts[] = {
			{ "verbose", 2, 0, OPT_VERBOSE },
			{ "help", 0, 0, OPT_HELP },
		};

		c = getopt_long(argc, argv, "+h", opts, &option_index);
		if (c == -1)
			break;
		switch(c) {
		case 'h':
		case OPT_HELP:
			usage();
			goto out;
		case OPT_VERBOSE:
			if (optarg && streq(optarg, "raw"))
				options.flags |= FLAG_VERBOSE_RAW;
			else
				options.flags |= FLAG_VERBOSE;
			break;
		default:
			goto out;
		}
	}

	if (optind >= argc) {
		rc = ERR_USAGE;
		goto out;
	}

	if (options.flags & FLAG_VERBOSE_RAW)
		ratbag_log_set_priority(ratbag, RATBAG_LOG_PRIORITY_RAW);
	else if (options.flags & FLAG_VERBOSE)
		ratbag_log_set_priority(ratbag, RATBAG_LOG_PRIORITY_DEBUG);

	argc -= optind;
	argv += optind;

	command = argv[0];
	rc = run_subcommand(command,
			    ratbag_commands,
			    ratbag,
			    &options,
			    argc, argv);
out:
	ratbag_resolution_unref(options.resolution);
	ratbag_profile_unref(options.profile);
	ratbag_device_unref(options.device);
	ratbag_unref(ratbag);

	if (rc == ERR_USAGE)
		usage();

	return rc;
}
