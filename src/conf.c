/* Parser for /etc/finit.conf and /etc/finit.d/<SVC>.conf
 *
 * Copyright (c) 2012-2015  Joachim Nilsson <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"		/* Generated by configure script */

#include <dirent.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <lite/lite.h>
#include <lite/queue.h>		/* BSD sys/queue.h API */
#include <sys/time.h>

#include "finit.h"
#include "cond.h"
#include "service.h"
#include "tty.h"
#include "helpers.h"

#define BOOTSTRAP (runlevel == 0)
#define MATCH_CMD(l, c, x) \
	(!strncasecmp(l, c, strlen(c)) && (x = (l) + strlen(c)))

struct rlimit global_rlimit[RLIMIT_NLIMITS];

struct conf_change {
	TAILQ_ENTRY(conf_change) link;
	char *name;
};

static uev_t w1, w2, w3;
static TAILQ_HEAD(head, conf_change) conf_change_list = TAILQ_HEAD_INITIALIZER(conf_change_list);

static int parse_conf(char *file);
static void drop_changes(void);

void conf_parse_cmdline(void)
{
	int dbg = 0;
	FILE *fp;

	fp = fopen("/proc/cmdline", "r");
	if (fp) {
		char line[LINE_SIZE];

		fgets(line, sizeof(line), fp);
		chomp(line);
		_d("%s", line);

		if (strstr(line, "finit_debug") || strstr(line, "--debug"))
			dbg = 1;

		fclose(fp);
	}

	log_init(dbg);
}

/* Convert optional "[!123456789S]" string into a bitmask */
int conf_parse_runlevels(char *runlevels)
{
	int i, not = 0, bitmask = 0;

	if (!runlevels)
		runlevels = "[234]";
	i = 1;
	while (i) {
		int level;
		char lvl = runlevels[i++];

		if (']' == lvl || 0 == lvl)
			break;
		if ('!' == lvl) {
			not = 1;
			bitmask = 0x3FE;
			continue;
		}

		if ('s' == lvl || 'S' == lvl)
			lvl = '0';

		level = lvl - '0';
		if (level > 9 || level < 0)
			continue;

		if (not)
			CLRBIT(bitmask, level);
		else
			SETBIT(bitmask, level);
	}

	return bitmask;
}

void conf_parse_cond(svc_t *svc, char *cond)
{
	size_t i = 0;
	char *ptr;

	if (!svc) {
		_e("Invalid service pointer");
		return;
	}

	/* By default we assume UNIX daemons support SIGHUP */
	if (svc_is_daemon(svc))
		svc->sighup = 1;

	if (!cond)
		return;

	/* First character must be '!' if SIGHUP is not supported. */
	ptr = cond;
	if (ptr[i] == '!') {
		svc->sighup = 0;
		ptr++;
	}

	while (ptr[i] != '>' && ptr[i] != 0)
		i++;
	ptr[i] = 0;

	if (i >= sizeof(svc->cond)) {
		logit(LOG_WARNING, "Too long event list in declaration of %s: %s", svc->cmd, ptr);
		return;
	}

	strlcpy(svc->cond, ptr, sizeof(svc->cond));
}

struct rlimit_name {
	char *name;
	int val;
};

static const struct rlimit_name rlimit_names[] = {
	{ "as",         RLIMIT_AS         },
	{ "core",       RLIMIT_CORE       },
	{ "cpu",        RLIMIT_CPU        },
	{ "data",       RLIMIT_DATA       },
	{ "fsize",      RLIMIT_FSIZE      },
	{ "locks",      RLIMIT_LOCKS      },
	{ "memlock",    RLIMIT_MEMLOCK    },
	{ "msgqueue",   RLIMIT_MSGQUEUE   },
	{ "nice",       RLIMIT_NICE       },
	{ "nofile",     RLIMIT_NOFILE     },
	{ "nproc",      RLIMIT_NPROC      },
	{ "rss",        RLIMIT_RSS        },
	{ "rtprio",     RLIMIT_RTPRIO     },
#ifdef RLIMIT_RTTIME
	{ "rttime",     RLIMIT_RTTIME     },
#endif
	{ "sigpending", RLIMIT_SIGPENDING },
	{ "stack",      RLIMIT_STACK      },

	{ NULL, 0 }
};

int str2rlim(char *str)
{
	const struct rlimit_name *rn;

	for (rn = rlimit_names; rn->name; rn++) {
		if (!strcmp(str, rn->name))
			return rn->val;
	}

	return -1;
}

char *rlim2str(int rlim)
{
	const struct rlimit_name *rn;

	for (rn = rlimit_names; rn->name; rn++) {
		if (rn->val == rlim)
			return rn->name;
	}

	return "unknown";
}

void conf_parse_rlimit(char *line, struct rlimit arr[])
{
	char *level, *limit, *val;
	int resource = -1;
	rlim_t cfg, *set;

	level = strtok(line, " \t");
	if (!level)
		goto error;

	limit = strtok(NULL, " \t");
	if (!limit)
		goto error;

	resource = str2rlim(limit);
	if (resource < 0 || resource > RLIMIT_NLIMITS)
		goto error;

	val = strtok(NULL, " \t");
	if (!val)
		goto error;

	if (!strcmp(level, "soft"))
		set = &arr[resource].rlim_cur;
	else if (!strcmp(level, "hard"))
		set = &arr[resource].rlim_max;
	else
		goto error;

	/* Official keyword from v3.1 is `unlimited`, from prlimit(1) */
	if (!strcmp(val, "unlimited") || !strcmp(val, "infinity")) {
		cfg = RLIM_INFINITY;
	} else {
		const char *err = NULL;

		cfg = strtonum(val, 0, (long long)2 << 31, &err);
		if (err) {
			logit(LOG_WARNING, "rlimit: invalid %s value: %s",
			      rlim2str(resource), val);
			return;
		}
	}

	*set = cfg;
	return;
error:
	logit(LOG_WARNING, "rlimit: parse error");
}

static void parse_static(char *line)
{
	char *x;
	char cmd[CMD_SIZE];

	if (BOOTSTRAP && MATCH_CMD(line, "host ", x)) {
		if (hostname) free(hostname);
		hostname = strdup(strip_line(x));
		return;
	}

	if (BOOTSTRAP && MATCH_CMD(line, "mknod ", x)) {
		char *dev = strip_line(x);

		strcpy(cmd, "mknod ");
		strlcat(cmd, dev, sizeof(cmd));
		run_interactive(cmd, "Creating device node %s", dev);

		return;
	}

	if (BOOTSTRAP && MATCH_CMD(line, "network ", x)) {
		if (network) free(network);
		network = strdup(strip_line(x));
		return;
	}

	if (BOOTSTRAP && MATCH_CMD(line, "runparts ", x)) {
		if (runparts) free(runparts);
		runparts = strdup(strip_line(x));
		return;
	}

	if (MATCH_CMD(line, "include ", x)) {
		char *file = strip_line(x);

		strlcpy(cmd, file, sizeof(cmd));
		if (!fexist(cmd)) {
			_e("Cannot find include file %s, absolute path required!", x);
			return;
		}

		parse_conf(cmd);
		return;
	}

	if (MATCH_CMD(line, "shutdown ", x)) {
		if (sdown) free(sdown);
		sdown = strdup(strip_line(x));
		return;
	}

	/*
	 * The desired runlevel to start when leaving bootstrap (S).
	 * Finit supports 1-9, but most systems only use 1-6, where
	 * 6 is reserved for reboot and 0 for halt/poweroff.
	 */
	if (BOOTSTRAP && MATCH_CMD(line, "runlevel ", x)) {
		char *token = strip_line(x);
		const char *err = NULL;

		cfglevel = strtonum(token, 1, 9, &err);
		if (err)
			cfglevel = RUNLEVEL;
		if (cfglevel < 1 || cfglevel > 9 || cfglevel == 6)
			cfglevel = 2; /* Fallback */
		return;
	}
}

static void parse_dynamic(char *line, struct rlimit rlimit[], char *file)
{
	char *x;
	char cmd[CMD_SIZE];

	/* Skip comments, i.e. lines beginning with # */
	if (MATCH_CMD(line, "#", x))
		return;

	/* Kernel module to load at bootstrap */
	if (MATCH_CMD(line, "module ", x)) {
		char *mod;

		if (runlevel != 0)
			return;

		mod = strip_line(x);
		strcpy(cmd, "modprobe ");
		strlcat(cmd, mod, sizeof(cmd));
		run_interactive(cmd, "Loading kernel module %s", mod);

		return;
	}

	/* Monitored daemon, will be respawned on exit */
	if (MATCH_CMD(line, "service ", x)) {
		service_register(SVC_TYPE_SERVICE, x, rlimit, file);
		return;
	}

	/* One-shot task, will not be respawned */
	if (MATCH_CMD(line, "task ", x)) {
		service_register(SVC_TYPE_TASK, x, rlimit, file);
		return;
	}

	/* Like task but waits for completion, useful w/ [S] */
	if (MATCH_CMD(line, "run ", x)) {
		service_register(SVC_TYPE_RUN, x, rlimit, file);
		return;
	}

	/* Classic inetd service */
	if (MATCH_CMD(line, "inetd ", x)) {
#ifdef INETD_ENABLED
		service_register(SVC_TYPE_INETD, x, rlimit, file);
#else
		_e("Finit built with inetd support disabled, cannot register service inetd %s!", x);
#endif
		return;
	}

	/* Read resource limits */
	if (MATCH_CMD(line, "rlimit ", x)) {
		conf_parse_rlimit(x, rlimit);
		return;
	}

	/* Regular or serial TTYs to run getty */
	if (MATCH_CMD(line, "tty ", x)) {
		tty_register(strip_line(x), rlimit, file);
		return;
	}
}

static void tabstospaces(char *line)
{
	if (!line)
		return;

	for (int i = 0; line[i]; i++) {
		if (line[i] == '\t')
			line[i] = ' ';
	}
}

static int parse_conf_dynamic(char *file)
{
	FILE *fp;
	struct rlimit rlimit[RLIMIT_NLIMITS];

	fp = fopen(file, "r");
	if (!fp) {
		_pe("Failed opening %s", file);
		return 1;
	}

	/* Prepare default limits for each service */
	memcpy(rlimit, global_rlimit, sizeof(rlimit));

	_d("Parsing %s <<<<<<", file);
	while (!feof(fp)) {
		char line[LINE_SIZE] = "";

		if (!fgets(line, sizeof(line), fp))
			continue;

		chomp(line);
		tabstospaces(line);
		_d("%s", line);

		parse_dynamic(line, rlimit, file);
	}

	fclose(fp);

	return 0;
}

static int parse_conf(char *file)
{
	FILE *fp;
	char line[LINE_SIZE] = "";

	/*
	 * Get current global limits, which may be overridden from both
	 * finit.conf, for Finit and its services like inetd+getty, and
	 * *.conf in finit.d/, for each service(s) listed there.
	 */
	for (int i = 0; i < RLIMIT_NLIMITS; i++)
		getrlimit(i, &global_rlimit[i]);

	fp = fopen(file, "r");
	if (!fp)
		return 1;

	_d("Parsing %s", file);
	while (!feof(fp)) {
		if (!fgets(line, sizeof(line), fp))
			continue;

		chomp(line);
		tabstospaces(line);
		_d("%s", line);

		parse_static(line);
		parse_dynamic(line, global_rlimit, NULL);
	}

	fclose(fp);

	/* Set global limits */
	for (int i = 0; i < RLIMIT_NLIMITS; i++) {
		if (setrlimit(i, &global_rlimit[i]) == -1)
			logit(LOG_WARNING, "rlimit: Failed setting %s", rlim2str(i));
	}

	return 0;
}

/*
 * Reload /etc/finit.conf and all *.conf in /etc/finit.d/
 */
int conf_reload(void)
{
	int i, num;
	struct dirent **e;

	/* Mark and sweep */
	svc_mark_dynamic();
	tty_mark();

	/* First, read /etc/finit.conf */
	parse_conf(FINIT_CONF);

	/* Next, read all *.conf in /etc/finit.d/ */
	num = scandir(rcsd, &e, NULL, alphasort);
	if (num < 0) {
		_d("Skipping %s, no files found ...", rcsd);
		return 0;
	}

	for (i = 0; i < num; i++) {
		char *name = e[i]->d_name;
		char  path[LINE_SIZE];
		size_t len;
		struct stat st;

		snprintf(path, sizeof(path), "%s/%s", rcsd, name);

		/* Check that it's an actual file ... beyond any symlinks */
		if (lstat(path, &st)) {
			_d("Skipping %s, cannot access: %s", path, strerror(errno));
			continue;
		}

		/* Skip directories */
		if (S_ISDIR(st.st_mode)) {
			_d("Skipping directory %s", path);
			continue;
		}

		/* Check for dangling symlinks */
		if (S_ISLNK(st.st_mode)) {
			char *rp;

			rp = realpath(path, NULL);
			if (!rp) {
				logit(LOG_WARNING, "Skipping %s, dangling symlink: %s", path, strerror(errno));
				continue;
			}

			free(rp);
		}

		/* Check that file ends with '.conf' */
		len = strlen(path);
		if (len < 6 || strcmp(&path[len - 5], ".conf")) {
			_d("Skipping %s, not a valid .conf ... ", path);
			continue;
		}

		parse_conf_dynamic(path);
	}

	while (num--)
		free(e[num]);
	free(e);

	/* Drop record of all .conf changes */
	drop_changes();

	/*
	 * Set host name, from %DEFHOST, *.conf or /etc/hostname.  The
	 * latter wins, if neither exists we default to "noname"
	 */
	set_hostname(&hostname);

	return 0;
}

static struct conf_change *conf_find(char *file)
{
	struct conf_change *node, *tmp;

	TAILQ_FOREACH_SAFE(node, &conf_change_list, link, tmp) {
		if (string_compare(node->name, file))
			return node;
	}

	return NULL;
}

static void drop_change(struct conf_change *node)
{
	if (!node)
		return;

	TAILQ_REMOVE(&conf_change_list, node, link);
	free(node->name);
	free(node);
}


static void drop_changes(void)
{
	struct conf_change *node, *tmp;

	TAILQ_FOREACH_SAFE(node, &conf_change_list, link, tmp)
		drop_change(node);
}

static int do_change(char *name, uint32_t mask)
{
	struct conf_change *node;

	node = conf_find(name);
	if (mask & (IN_DELETE | IN_MOVED_FROM)) {
		drop_change(node);
		return 0;
	}

	if (node) {
		_d("Event already registered for %s ...", name);
		return 0;
	}

	node = malloc(sizeof(*node));
	if (!node)
		return 1;

	node->name = strdup(name);
	if (!node->name) {
		free(node);
		return 1;
	}

	TAILQ_INSERT_HEAD(&conf_change_list, node,link);

	return 0;
}

int conf_changed(char *file)
{
	char *ptr;

	if (!file)
		return 0;

	if ((ptr = strrchr(file, '/')))
		file = ++ptr;

	if (conf_find(file))
		return 1;

	return 0;
}

static void conf_cb(uev_t *w, void *arg, int events)
{
	static char ev_buf[8 *(sizeof(struct inotify_event) + NAME_MAX + 1) + 1];
	struct inotify_event *ev;
	ssize_t sz, len;

	sz = read(w->fd, ev_buf, sizeof(ev_buf) - 1);
	if (sz <= 0) {
		_pe("invalid inotify event");
		return;
	}
	ev_buf[sz] = 0;
	ev = (struct inotify_event *)ev_buf;

	if (arg) {
		do_change(arg, ev->mask);
		return;
	}

	for (ev = (void *)ev_buf; sz > (ssize_t)sizeof(*ev);
	     len = sizeof(*ev) + ev->len, ev = (void *)ev + len, sz -= len) {
		if (do_change(ev->name, ev->mask)) {
			_pe("conf_monitor: Out of memory");
			break;
		}
	}
}

static int add_watcher(uev_ctx_t *ctx, uev_t *w, char *path, uint32_t opt)
{
	struct stat st;
	uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_MOVE;
	char *arg = NULL;
	int fd, wd;

	if (!ctx)
		return 0;

	if (stat(path, &st)) {
		_d("No such file or directory, skipping %s", path);
		w->fd = -1;
		return 0;
	}
	if (!S_ISDIR(st.st_mode)) {
		arg = strrchr(path, '/');
		if (!arg)
			arg = path;
		else
			arg++;
	}

	if (w->fd >= 0)
		close(w->fd);

	fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0) {
		_pe("Failed creating inotify descriptor");
		w->fd = -1;
		return 1;
	}

	/*
	 * Only forward error, don't report error,
	 * user may not have @path and that's OK
	 */
	wd = inotify_add_watch(fd, path, mask | opt);
	if (wd < 0) {
		w->fd = -1;
		close(fd);
		return 1;
	}

	if (uev_io_init(ctx, w, conf_cb, arg, fd, UEV_READ)) {
		_pe("Failed setting up I/O callback for %s watcher", path);
		w->fd = -1;
		close(fd);
		return 1;
	}

	return 0;
}

/*
 * Set up inotify watcher and load all *.conf in /etc/finit.d/
 */
int conf_monitor(uev_ctx_t *ctx)
{
	int rc = 0;

	/*
	 * If only one watcher fails, that's OK.  A user may have only
	 * one of /etc/finit.conf or /etc/finit.d in use, and may also
	 * have or not have symlinks in place.  We need to monitor for
	 * changes to either symlink or target.
	 */
	rc += add_watcher(ctx, &w1, FINIT_RCSD, 0);
	rc += add_watcher(ctx, &w2, FINIT_RCSD "/available", IN_DONT_FOLLOW);
	rc += add_watcher(ctx, &w3, FINIT_CONF, 0);

	return rc + conf_reload();
}

/*
 * Prepare .conf parser and load all .conf files
 */
int conf_init(void)
{
	hostname = strdup(DEFHOST);
	w1.fd = w2.fd = w3.fd = -1;

	return conf_monitor(NULL);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
