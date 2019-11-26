/* Finit state machine
 *
 * Copyright (c) 2016  Jonas Johansson <jonas.johansson@westermo.se>
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

#include <sys/types.h>

#include "finit.h"
#include "cond.h"
#include "conf.h"
#include "helpers.h"
#include "private.h"
#include "service.h"
#include "sig.h"
#include "tty.h"
#include "sm.h"
#include "utmp-api.h"

sm_t sm;

void sm_init(sm_t *sm)
{
	sm->state = SM_BOOTSTRAP_STATE;
	sm->newlevel = -1;
	sm->reload = 0;
	sm->in_teardown = 0;
}

static char *sm_status(sm_state_t state)
{
	switch (state) {
	case SM_BOOTSTRAP_STATE:
		return "bootstrap";

	case SM_RUNNING_STATE:
		return "running";

	case SM_RUNLEVEL_CHANGE_STATE:
		return "runlevel/change";

	case SM_RUNLEVEL_WAIT_STATE:
		return "runlevel/wait";

	case SM_RELOAD_CHANGE_STATE:
		return "reload/change";

	case SM_RELOAD_WAIT_STATE:
		return "reload/wait";

	default:
		return "unknown";
	}
}

/*
 * Disable login in single user mode and shutdown/reboot
 *
 * Re-enable only when going from these runlevels, this way a user can
 * manage /etc/nologin manually within the other runlevels without us
 * pulling the rug from under their feet.
 */
static void nologin(void)
{
	if (runlevel == 1 || runlevel == 0 || runlevel == 6)
		touch("/etc/nologin");

	if (prevlevel == 1 || prevlevel == 0 || prevlevel == 6)
		erase("/etc/nologin");
}

void sm_set_runlevel(sm_t *sm, int newlevel)
{
	sm->newlevel = newlevel;
}

void sm_set_reload(sm_t *sm)
{
	sm->reload = 1;
}

int sm_is_in_teardown(sm_t *sm)
{
	return sm->in_teardown;
}

void sm_step(sm_t *sm)
{
	svc_t *svc;
	sm_state_t old_state;

restart:
	old_state = sm->state;

	_d("state: %s, runlevel: %d, newlevel: %d, teardown: %d, reload: %d",
	   sm_status(sm->state), runlevel, sm->newlevel, sm->in_teardown, sm->reload);

	switch (sm->state) {
	case SM_BOOTSTRAP_STATE:
		_d("Bootstrapping all services in runlevel S from %s", FINIT_CONF);
		service_step_all(SVC_TYPE_RUN | SVC_TYPE_TASK | SVC_TYPE_SERVICE);
		sm->state = SM_RUNNING_STATE;
		break;

	case SM_RUNNING_STATE:
		/* runlevel changed? */
		if (sm->newlevel >= 0 && sm->newlevel <= 9) {
			if (runlevel == sm->newlevel) {
				sm->newlevel = -1;
				break;
			}
			sm->state = SM_RUNLEVEL_CHANGE_STATE;
			break;
		}
		/* reload ? */
		if (sm->reload) {
			sm->reload = 0;
			sm->state = SM_RELOAD_CHANGE_STATE;
		}
		break;

	case SM_RUNLEVEL_CHANGE_STATE:
		prevlevel    = runlevel;
		runlevel     = sm->newlevel;
		sm->newlevel = -1;

		/* Restore terse mode and run hooks before shutdown */
		if (runlevel == 0 || runlevel == 6) {
			log_exit();
			plugin_run_hooks(HOOK_SHUTDOWN);
		}

		_d("Setting new runlevel --> %d <-- previous %d", runlevel, prevlevel);
		logit(LOG_CONSOLE | LOG_NOTICE, "%s, entering runlevel %d", INIT_HEADING, runlevel);
		runlevel_set(prevlevel, runlevel);

		/* Disable login in single-user mode as well as shutdown/reboot */
		nologin();

		/* Make sure to (re)load all *.conf in /etc/finit.d/ */
		if (conf_any_change())
			conf_reload();

		/* Reset once flag of runtasks */
		service_runtask_clean();

		_d("Stopping services not allowed in new runlevel ...");
		sm->in_teardown = 1;
		service_step_all(SVC_TYPE_ANY);

		sm->state = SM_RUNLEVEL_WAIT_STATE;
		break;

	case SM_RUNLEVEL_WAIT_STATE:
		/*
		 * Need to wait for any services to stop? If so, exit early
		 * and perform second stage from service_monitor later.
		 */
		svc = svc_stop_completed();
		if (svc) {
			_d("Waiting to collect %s(%d) ...", svc->cmd, svc->pid);
			break;
		}

		/* Prev runlevel services stopped, call hooks before starting new runlevel ... */
		_d("All services have been stoppped, calling runlevel change hooks ...");
		plugin_run_hooks(HOOK_RUNLEVEL_CHANGE);  /* Reconfigure HW/VLANs/etc here */

		_d("Starting services new to this runlevel ...");
		sm->in_teardown = 0;
		service_step_all(SVC_TYPE_ANY);

		/* Cleanup stale services */
		svc_clean_dynamic(service_unregister);

		/*
		 * "I've seen things you people wouldn't believe.  Attack ships on fire off
		 *  the shoulder of Orion.  I watched C-beams glitter in the dark near the
		 *  Tannhäuser Gate.  All those .. moments .. will be lost in time, like
		 *  tears ... in ... rain."
		 */
		if (runlevel == 0 || runlevel == 6) {
			do_shutdown(halt);
			sm->state = SM_RUNNING_STATE;
			break;
		}

		/* No TTYs run at bootstrap, they have a delayed start. */
		if (prevlevel > 0)
			tty_runlevel();

		sm->state = SM_RUNNING_STATE;
		break;

	case SM_RELOAD_CHANGE_STATE:
		/* First reload all *.conf in /etc/finit.d/ */
		conf_reload();

		/*
		 * Then, mark all affected service conditions as in-flux and
		 * let all affected services move to WAITING/HALTED
		 */
		_d("Stopping services not allowed after reconf ...");
		sm->in_teardown = 1;
		cond_reload();
		service_step_all(SVC_TYPE_SERVICE | SVC_TYPE_INETD);
		tty_reload(NULL);

		sm->state = SM_RELOAD_WAIT_STATE;
		break;

	case SM_RELOAD_WAIT_STATE:
		/*
		 * Need to wait for any services to stop? If so, exit early
		 * and perform second stage from service_monitor later.
		 */
		svc = svc_stop_completed();
		if (svc) {
			_d("Waiting to collect %s(%d) ...", svc->cmd, svc->pid);
			break;
		}

		sm->in_teardown = 0;
		/* Cleanup stale services */
		svc_clean_dynamic(service_unregister);

		_d("Starting services after reconf ...");
		service_step_all(SVC_TYPE_SERVICE | SVC_TYPE_INETD);

		_d("Calling reconf hooks ...");
		plugin_run_hooks(HOOK_SVC_RECONF);

		service_step_all(SVC_TYPE_SERVICE | SVC_TYPE_INETD);
		_d("Reconfiguration done");

		sm->state = SM_RUNNING_STATE;
		break;
	}

	if (sm->state != old_state)
		goto restart;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
