// SPDX-License-Identifier: MIT
/*
 * aurora-display-light: the built-in display's brightness follows the room's
 * light, on Apple silicon MacBooks.
 *
 * It runs in the user's session. The light sensor is read from sysfs; the
 * brightness is set through the desktop (panel.h), so the desktop's own
 * brightness keys, slider, idle dimming and on-screen indicator keep working.
 * A level the user picks is learned: the curve moves to pass through it.
 *
 * Settings live in ~/.config/aurora-display-lightrc and are re-read when the
 * file changes; the adjustments learned from the user live in
 * ~/.local/state/aurora-display-light/learned. A short status is kept in
 * $XDG_RUNTIME_DIR/aurora-display-light/status for settings pages.
 */
#include <errno.h>
#include <glob.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "model.h"
#include "panel.h"
#include "plasma.h"
#include "settings.h"

#define SAMPLE_S 0.2		/* the light sensor reports at 5 Hz */
#define TICK_S 0.033		/* steps of a transition */
#define SETTLE_S 1.0		/* someone else's change: wait until the level stays this long */
#define GRACE_S 10.0		/* after start or wake, a change is held, not learned: it may be a restore */
#define DARK_CHECK_S 1.0
#define STATUS_S 1.0
#define KDE_CHECK_S 5.0	/* while setting fails, how often to look for Plasma again */

static volatile sig_atomic_t stop;
static bool verbose;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

#define info(...) do { if (verbose) say(__VA_ARGS__); } while (0)

static double clock_s(clockid_t id)
{
	struct timespec ts;

	clock_gettime(id, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double now_s(void)
{
	return clock_s(CLOCK_MONOTONIC);
}

/* The first IIO device with a processed illuminance channel. */
static char *find_als(void)
{
	glob_t g;
	char *path = NULL;

	if (glob("/sys/bus/iio/devices/iio:device*/in_illuminance_input", 0, NULL, &g) == 0 &&
	    g.gl_pathc)
		path = strdup(g.gl_pathv[0]);
	globfree(&g);
	return path;
}

static int read_lux(const char *path, double *lux)
{
	char buf[64], *end;
	FILE *f = fopen(path, "re");

	if (!f)
		return -errno;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -EIO;
	}
	fclose(f);
	*lux = strtod(buf, &end);
	return end == buf || !(*lux >= 0) ? -EINVAL : 0;
}

/* System-wide defaults first (the first of XDG_CONFIG_DIRS), then the user's file. */
static void load_settings(struct dl_settings *s)
{
	const char *dirs = getenv("XDG_CONFIG_DIRS");
	char path[PATH_MAX];
	size_t n = dirs && *dirs == '/' ? strcspn(dirs, ":") : 0;

	dl_settings_defaults(s);
	if (n)
		snprintf(path, sizeof(path), "%.*s/" DL_CONFIG_NAME, (int)n, dirs);
	else
		snprintf(path, sizeof(path), "/etc/xdg/" DL_CONFIG_NAME);
	dl_settings_read(s, path);
	if (!dl_config_path(path, sizeof(path)))
		dl_settings_read(s, path);
}

static bool contains_word(const char *list, const char *word)
{
	char buf[256], *save = NULL;

	if (!list)
		return false;
	snprintf(buf, sizeof(buf), "%s", list);
	for (char *w = strtok_r(buf, ":", &save); w; w = strtok_r(NULL, ":", &save))
		if (!strcasecmp(w, word))
			return true;
	return false;
}

/* mkdir -p for the directory a file is in. */
static void make_parent(const char *file)
{
	char dir[PATH_MAX];

	snprintf(dir, sizeof(dir), "%s", file);
	for (char *p = dir + 1; (p = strchr(p, '/')); p++) {
		*p = '\0';
		mkdir(dir, 0700);
		*p = '/';
	}
}

/* Watch the directory a file is in: editors and KConfig replace files by renaming. */
static int watch_dir_of(int fd, const char *file)
{
	char dir[PATH_MAX];

	make_parent(file);
	snprintf(dir, sizeof(dir), "%s", file);
	return inotify_add_watch(fd, dirname(dir),
				 IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE);
}

/* Which of the files named (basenames, NULL-terminated) changed: bit i for names[i]. */
static unsigned drain_inotify(int fd, const char *const *names)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	unsigned changed = 0;
	ssize_t n;

	while ((n = read(fd, buf, sizeof(buf))) > 0)
		for (char *p = buf; p < buf + n;) {
			const struct inotify_event *ev = (const struct inotify_event *)p;

			for (unsigned i = 0; ev->len && names[i]; i++)
				if (!strcmp(ev->name, names[i]))
					changed |= 1u << i;
			p += sizeof(*ev) + ev->len;
		}
	return changed;
}

struct status {
	const char *state;
	double lux, ambient, target, nits, max;
	const char *profile, *response, *backend;
};

static void write_status(const struct status *st)
{
	const char *run = getenv("XDG_RUNTIME_DIR");
	char dir[PATH_MAX - 16], path[PATH_MAX], tmp[PATH_MAX];
	FILE *f;

	if (!run || *run != '/')
		return;
	snprintf(dir, sizeof(dir), "%s/aurora-display-light", run);
	mkdir(dir, 0700);
	snprintf(path, sizeof(path), "%s/status", dir);
	snprintf(tmp, sizeof(tmp), "%s/status.new", dir);
	f = fopen(tmp, "we");
	if (!f)
		return;
	fprintf(f, "state=%s\nbackend=%s\nprofile=%s\nresponse=%s\n", st->state, st->backend,
		st->profile, st->response);
	fprintf(f, "lux=%.1f\nambient=%.1f\ntarget=%.1f\nnits=%.1f\nmax=%.0f\n", st->lux,
		st->ambient, st->target, st->nits, st->max);
	if (fclose(f) == 0)
		rename(tmp, path);
}

static void reload(struct dl_controller *c, struct dl_settings *s, const struct panel *pn,
		   char *state_path, double t)
{
	struct dl_params p;
	double offset[DL_POINTS];

	load_settings(s);
	dl_settings_params(s, pn->max_nits, &p);
	dl_learned_load(state_path, s->profile, offset);
	/* our own save of what was learned comes back here too: nothing to do then */
	if (!memcmp(&p, &c->p, sizeof(p)) && !memcmp(offset, c->offset, sizeof(offset)))
		return;
	dl_controller_set_params(c, t, &p, offset);
	info("settings: %s, profile %s, response %s%s", s->enabled ? "on" : "off", s->profile,
	     s->response, s->learn ? ", learning" : "");
	if (c->ramp.active && c->ramp.start == t)
		info("%.1f lx: %.0f -> %.0f nits over %.1f s", c->ambient.ambient,
		     dl_nits(&c->p, c->ramp.from), c->target, c->ramp.length);
}

static int run(const char *trace_path)
{
	struct dl_settings s;
	struct dl_params params;
	struct dl_controller c;
	struct panel pn;
	char state_path[PATH_MAX] = "", config_path[PATH_MAX] = "", outputs_path[PATH_MAX] = "";
	const char *desktop = getenv("XDG_CURRENT_DESKTOP"), *state_name;
	double offset[DL_POINTS], t0 = now_s(), lux = NAN;
	double sample_t = -1, dark_t = -1, status_t = -1, grace_until, suspended;
	bool dark = false, kde, kde_guessed, was_ready = false, foreign_seen = false, plasma_auto = false;
	bool set_failing = false;
	double kde_check_t = -1;
	int exit_code = 0;
	FILE *trace = NULL;
	char *als;
	const char *watched[4] = { DL_CONFIG_NAME, NULL, DL_KWIN_OUTPUTS_NAME, NULL };
	unsigned changed;
	int ino, ret;

	if (contains_word(desktop, "GNOME")) {
		say("GNOME sets the screen brightness from the light sensor itself "
		    "(Settings, Power, Automatic Screen Brightness); nothing to do");
		return 0;
	}
	/* Plasma, also when the session didn't export XDG_CURRENT_DESKTOP to the user manager */
	kde_guessed = !desktop || !*desktop;
	kde = contains_word(desktop, "KDE") || (kde_guessed && panel_kde_running());
	load_settings(&s);
	als = find_als();
	if (!als) {
		say("no ambient light sensor; nothing to do");
		return 0;
	}
	ret = panel_open(&pn, s.backlight, kde);
	if (ret == -ENODEV) {
		say("no built-in Apple panel backlight; nothing to do");
		return 0;
	}
	if (ret < 0) {
		say("cannot reach the desktop's brightness control: %s", strerror(-ret));
		return 1;
	}
	dl_config_path(config_path, sizeof(config_path));
	dl_state_path(state_path, sizeof(state_path));
	state_name = strrchr(state_path, '/') ? strrchr(state_path, '/') + 1 : state_path;
	watched[1] = state_name;
	/* Plasma's own automatic brightness, when it is on, has the panel */
	if (kde && !dl_plasma_outputs_path(outputs_path, sizeof(outputs_path)))
		plasma_auto = dl_plasma_auto_brightness(outputs_path);
	else
		watched[2] = NULL;
	dl_settings_params(&s, pn.max_nits, &params);
	dl_learned_load(state_path, s.profile, offset);
	dl_controller_init(&c, &params, offset, -1);

	ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ino >= 0) {
		if (config_path[0])
			watch_dir_of(ino, config_path);
		if (state_path[0])
			watch_dir_of(ino, state_path);
		if (outputs_path[0])
			watch_dir_of(ino, outputs_path);
	}
	if (trace_path) {
		make_parent(trace_path);
		trace = fopen(trace_path, "we");
		if (trace)
			fprintf(trace, "t,lux,ambient,target,set_nits,desktop_nits,backlight_nits,event\n");
		else
			say("cannot write the trace to %s: %s", trace_path, strerror(errno));
	}
	say("panel %s (%.0f nits), light sensor %s, brightness through %s; profile %s, response %s%s",
	    pn.sysfs, pn.max_nits, als, kde ? "Plasma" : "logind", s.profile, s.response,
	    s.learn ? ", learning" : "");
	if (plasma_auto)
		say("Plasma's own automatic brightness is on for the built-in screen; leaving the brightness to it");

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	grace_until = t0 + GRACE_S;
	suspended = clock_s(CLOCK_BOOTTIME) - t0;

	while (!stop) {
		double t = now_s(), susp = clock_s(CLOCK_BOOTTIME) - t, wait = SAMPLE_S, nits;
		const char *event = "";
		struct pollfd fds[2];
		int nfds = 0;
		short events = 0;
		int bus_fd;

		/* After a wake the light is different and the desktop restores its own level. */
		if (susp - suspended > 2) {
			grace_until = t + GRACE_S;
			dl_controller_restart(&c);
			was_ready = false;
			info("woke up");
		}
		suspended = susp;
		changed = ino >= 0 ? drain_inotify(ino, watched) : 0;
		if (changed & 3) {
			reload(&c, &s, &pn, state_path, t);
			event = "settings";
		}
		if (changed & 4 && dl_plasma_auto_brightness(outputs_path) != plasma_auto) {
			plasma_auto = !plasma_auto;
			info("%s", plasma_auto ? "Plasma's own automatic brightness was turned on; leaving the brightness to it"
					 : "Plasma's own automatic brightness was turned off; following the light again");
			event = "settings";
		}
		panel_process(&pn, t);

		if (!s.enabled || plasma_auto || !panel_ready(&pn, t)) {
			pn.foreign = false;
			was_ready = false;
			dl_controller_restart(&c);
			if (t - status_t >= STATUS_S) {
				write_status(&(struct status){ plasma_auto ? "plasma" : s.enabled ? "waiting" : "off", lux, NAN, NAN,
						panel_level(&pn), pn.max_nits, s.profile, s.response,
						kde ? "plasma" : "logind" });
				status_t = t;
			}
			wait = 1;
			goto sleep;
		}
		if (!was_ready) {
			/* start from the level the desktop shows */
			c.shown = panel_level(&pn);
			c.automatic = -1;
			pn.foreign = false;
			was_ready = true;
		}
		if (t - dark_t >= DARK_CHECK_S) {
			bool d = panel_dark(&pn);

			dark_t = t;
			if (d != dark)
				info("%s", d ? "lid closed or displays off: paused" : "display on again");
			if (!d && dark) {
				c.shown = panel_level(&pn);
				grace_until = t + GRACE_S;
			}
			dark = d;
		}
		if (dark) {
			dl_controller_restart(&c);
			pn.foreign = false;
			wait = DARK_CHECK_S;
			goto sleep;
		}

		if (pn.foreign) {
			/* Someone else is changing it: stand back until the level settles. */
			if (c.ramp.active || !foreign_seen)
				info("changed to %.0f nits by %s", pn.foreign_nits, pn.foreign_by);
			foreign_seen = true;
			c.ramp.active = false;
			if (t - pn.foreign_t >= SETTLE_S) {
				/*
				 * Learned: a change from the keys, the slider or another program
				 * under Plasma. Held: the desktop's own levels (a power profile's),
				 * a change nothing says the source of, a level the desktop restores
				 * at start or after a wake, and anything on desktops that can't say
				 * where a change came from.
				 */
				enum dl_adjust how = pn.backend == PANEL_KDE && s.learn && t >= grace_until &&
							     pn.foreign_person ? DL_ADJUST_LEARN : DL_ADJUST_HOLD;
				double recent = dl_ambient_recent(&c.ambient);

				pn.foreign = false;
				foreign_seen = false;
				/*
				 * A step of a transition that was on its way when the change
				 * came in may have landed after it: put the chosen level back.
				 */
				if (fabs(panel_level(&pn) - pn.foreign_nits) > 0.5)
					panel_set(&pn, pn.foreign_nits);
				if (dl_controller_adjusted(&c, t, pn.foreign_nits, how)) {
					dl_learned_save(state_path, s.profile, c.offset);
					/* as saved (rounded), so reading it back changes nothing */
					dl_learned_load(state_path, s.profile, c.offset);
					info("you set %.0f nits at %.1f lx: learned", pn.foreign_nits, recent);
					event = "learned";
				} else if (c.hold) {
					info("set to %.0f nits at %.1f lx by %s: holding", pn.foreign_nits, recent,
					     pn.foreign_by);
					event = "hold";
				} else {
					info("back to %.0f nits: automatic", pn.foreign_nits);
					event = "automatic";
				}
			} else {
				wait = SETTLE_S / 4;
				goto sleep;
			}
		}

		if (sample_t < 0 || t - sample_t >= SAMPLE_S - 0.005) {
			sample_t = t;
			if (read_lux(als, &lux) == 0 && dl_controller_sample(&c, t, lux)) {
				info("%.1f lx: %.0f -> %.0f nits over %.1f s", c.ambient.ambient,
				     dl_nits(&c.p, c.ramp.from), c.target, c.ramp.length);
				event = "transition";
			}
			if (trace)
				fprintf(trace, "%.3f,%.2f,%.2f,%.1f,%.1f,%.1f,%.0f,%s\n", t - t0, lux,
					c.ambient.ambient, c.target, c.shown, panel_level(&pn),
					panel_sysfs_level(&pn), event);
		}
		if (dl_controller_level(&c, t, &nits)) {
			ret = panel_set(&pn, nits);
			if (ret >= 0) {
				if (set_failing)
					say("setting the brightness works again");
				set_failing = false;
			} else {
				if (!set_failing)
					say("setting %.0f nits failed: %s", nits, strerror(-ret));
				set_failing = true;
				/*
				 * Plasma's brightness control was not up when this started
				 * (PowerDevil starting late or restarting): exit, and the
				 * service restarts on it.
				 */
				if (!kde && kde_guessed && (kde_check_t < 0 || t - kde_check_t >= KDE_CHECK_S)) {
					kde_check_t = t;
					if (panel_kde_running()) {
						say("Plasma's brightness control is up now; restarting to use it");
						exit_code = 1;
						break;
					}
				}
			}
			wait = TICK_S;
		}
		if (t - status_t >= STATUS_S) {
			write_status(&(struct status){ c.hold ? "holding" : "following", lux,
					c.ambient.ambient, c.target, c.shown, pn.max_nits, s.profile,
					s.response, kde ? "plasma" : "logind" });
			status_t = t;
		}
sleep:
		if (trace)
			fflush(trace);
		bus_fd = panel_fd(&pn, &events);
		if (bus_fd >= 0)
			fds[nfds++] = (struct pollfd){ .fd = bus_fd, .events = events };
		if (ino >= 0)
			fds[nfds++] = (struct pollfd){ .fd = ino, .events = POLLIN };
		poll(fds, nfds, (int)ceil(wait * 1000));
	}
	write_status(&(struct status){ "stopped", lux, NAN, NAN, NAN, pn.max_nits, s.profile,
			s.response, kde ? "plasma" : "logind" });
	if (trace)
		fclose(trace);
	if (ino >= 0)
		close(ino);
	panel_close(&pn);
	free(als);
	return exit_code;
}

/*
 * Run the model over a recorded light trace: CSV lines of "t,lux" with an
 * optional third column, a level in nits the user picked at that time.
 */
static int replay(const char *path, const char *profile, const char *response, double max,
		  double start)
{
	struct dl_settings s;
	struct dl_params p;
	struct dl_controller c;
	char line[256];
	int transitions = 0, learned = 0;
	FILE *f = strcmp(path, "-") ? fopen(path, "re") : stdin;

	if (!f) {
		say("%s: %s", path, strerror(errno));
		return 1;
	}
	dl_settings_defaults(&s);
	if (profile)
		snprintf(s.profile, sizeof(s.profile), "%s", profile);
	if (response)
		snprintf(s.response, sizeof(s.response), "%s", response);
	dl_settings_params(&s, max, &p);
	dl_controller_init(&c, &p, NULL, start);
	while (fgets(line, sizeof(line), f)) {
		double t, lux = NAN, pick = NAN, nits;
		char *a = line, *b, *end;

		t = strtod(a, &end);
		if (end == a || *end != ',')
			continue;
		b = end + 1;
		lux = strtod(b, &end);
		if (end == b)
			lux = NAN;
		if (*end == ',')
			pick = strtod(end + 1, NULL);
		dl_controller_level(&c, t, &nits);
		if (pick > 0) {
			learned += dl_controller_adjusted(&c, t, pick, DL_ADJUST_LEARN);
			printf("%8.1f s  picked %.0f nits at %.1f lx\n", t, pick, dl_ambient_recent(&c.ambient));
		}
		if (lux >= 0 && dl_controller_sample(&c, t, lux)) {
			transitions++;
			printf("%8.1f s  %.1f lx: %.0f -> %.0f nits over %.1f s\n", t, c.ambient.ambient,
			       dl_nits(&c.p, c.ramp.from), c.target, c.ramp.length);
		}
	}
	if (f != stdin)
		fclose(f);
	printf("%d transitions, %d learned\n", transitions, learned);
	return 0;
}

static void usage(void)
{
	printf("Usage: aurora-display-light [-v] [--trace FILE]\n"
	       "       aurora-display-light --replay FILE [--profile NAME] [--response NAME]\n"
	       "                            [--max NITS] [--start NITS]\n"
	       "\n"
	       "Sets the built-in display's brightness from the ambient light sensor.\n"
	       "Settings: ~/.config/" DL_CONFIG_NAME ".\n"
	       "\n"
	       "  -v, --verbose   log every transition and every change made by others\n"
	       "  --trace FILE    write each light sample and level to FILE (CSV)\n"
	       "  --replay FILE   run the model over a CSV of t,lux[,picked nits] and print what it does\n");
}

int main(int argc, char **argv)
{
	const char *trace = NULL, *replay_path = NULL, *profile = NULL, *response = NULL;
	double max = 500, start = 150;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
			verbose = true;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage();
			return 0;
		} else if (next && !strcmp(a, "--trace")) {
			trace = argv[++i];
		} else if (next && !strcmp(a, "--replay")) {
			replay_path = argv[++i];
		} else if (next && !strcmp(a, "--profile")) {
			profile = argv[++i];
		} else if (next && !strcmp(a, "--response")) {
			response = argv[++i];
		} else if (next && !strcmp(a, "--max")) {
			max = atof(argv[++i]);
		} else if (next && !strcmp(a, "--start")) {
			start = atof(argv[++i]);
		} else {
			usage();
			return 2;
		}
	}
	if (replay_path)
		return replay(replay_path, profile, response, max, start);
	setvbuf(stderr, NULL, _IOLBF, 0);
	return run(trace);
}
