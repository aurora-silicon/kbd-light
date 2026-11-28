// SPDX-License-Identifier: MIT
/*
 * aurora-kbd-light: drive the keyboard backlight from the ambient light
 * sensor, as macOS does.
 *
 * The level follows the room: brightest in the dark, fading out as the room
 * gets brighter, off in daylight. A change made by anyone else (the desktop's
 * slider, idle dimming) is respected: setting it to zero keeps it off until it
 * is turned on again, and any other level holds until the room's light
 * changes a lot. With the lid closed or the displays asleep the backlight
 * is off, and the light sensor in the lid is not read, so the level comes
 * back as it was. It dims after a time without a key press and
 * comes back at the next one.
 *
 * Only sysfs and evdev are used, so this needs nothing beyond libc.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define TICK_MS 250		/* sensor poll while steady */
#define FADE_TICK_MS 40		/* LED steps while fading */
#define FADE_MS 600		/* duration of a full-range fade */
#define SMOOTH_S 2.0		/* time constant of the lux average */
#define DEADBAND 3		/* ignore smaller level changes */
#define HOLD_RATIO 4.0		/* light change that ends a manual hold */
#define DISPLAYS_OFF_S 1.0	/* displays dark this long: backlight off */
#define SETTLE_RISE 1.25	/* a rise this large: the lid is still opening */
#define SETTLE_STEADY_MS (4 * TICK_MS)	/* no such rise this long: reading the room */
#define DEFAULT_IDLE_S 30	/* idle keyboard timeout; 0 disables it */
#define RESCAN_S 30		/* look for new keyboards this often */
#define MAX_KEYBOARDS 16
#define LONG_BITS (8 * sizeof(long))

struct config {
	double full_lux;	/* at or below: the brightest automatic level */
	double on_lux;		/* below: the backlight comes on */
	double off_lux;		/* above: it goes off again */
	int min_pct;		/* level just after it comes on */
	int max_pct;		/* level in the dark */
	double idle_s;		/* no key press this long: dim (0: never) */
	int idle_pct;		/* level while dimmed */
	bool verbose;
};

static volatile sig_atomic_t stop;

/* Event devices with letter keys: the built-in keyboard and external ones. */
static int kbd_fd[MAX_KEYBOARDS];
static int kbd_count;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static double env_double(const char *name, double def)
{
	const char *v = getenv(name);
	char *end;
	double d;

	if (!v || !*v)
		return def;
	d = strtod(v, &end);
	return (*end || d < 0) ? def : d;
}

static int read_file(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -errno;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -errno;
	buf[n] = '\0';
	return 0;
}

static int read_int(const char *path, long *out)
{
	char buf[64], *end;
	int ret = read_file(path, buf, sizeof(buf));

	if (ret)
		return ret;
	*out = strtol(buf, &end, 10);
	return end == buf ? -EINVAL : 0;
}

static int write_int(const char *path, long value)
{
	char buf[32];
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	int len, ret = 0;

	if (fd < 0)
		return -errno;
	len = snprintf(buf, sizeof(buf), "%ld\n", value);
	if (write(fd, buf, len) != len)
		ret = -errno;
	close(fd);
	return ret;
}

/* The first LED class device named like a keyboard backlight. */
static char *find_led(void)
{
	glob_t g;
	char *path = NULL;

	if (glob("/sys/class/leds/*kbd_backlight", 0, NULL, &g) == 0 && g.gl_pathc)
		path = strdup(g.gl_pathv[0]);
	globfree(&g);
	return path;
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
	int ret = read_file(path, buf, sizeof(buf));

	if (ret)
		return ret;
	*lux = strtod(buf, &end);
	return (end == buf || *lux < 0) ? -EINVAL : 0;
}

/* An event device reporting the lid switch, or -1. */
static int open_lid(void)
{
	unsigned long bits[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
	struct dirent *de;
	DIR *dir = opendir("/dev/input");
	int fd = -1;

	if (!dir)
		return -1;
	while ((de = readdir(dir)) && fd < 0) {
		char path[300];
		int cand;

		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		cand = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
		if (cand < 0)
			continue;
		memset(bits, 0, sizeof(bits));
		if (ioctl(cand, EVIOCGBIT(EV_SW, sizeof(bits)), bits) >= 0 &&
		    (bits[SW_LID / (8 * sizeof(long))] >> (SW_LID % (8 * sizeof(long)))) & 1)
			fd = cand;
		else
			close(cand);
	}
	closedir(dir);
	return fd;
}

static bool lid_closed(int fd)
{
	unsigned long bits[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };

	if (fd < 0 || ioctl(fd, EVIOCGSW(sizeof(bits)), bits) < 0)
		return false;
	return (bits[SW_LID / (8 * sizeof(long))] >> (SW_LID % (8 * sizeof(long)))) & 1;
}

static bool test_bit(const unsigned long *bits, int bit)
{
	return (bits[bit / LONG_BITS] >> (bit % LONG_BITS)) & 1;
}

static void close_keyboards(void)
{
	for (int i = 0; i < kbd_count; i++)
		if (kbd_fd[i] >= 0)
			close(kbd_fd[i]);
	kbd_count = 0;
}

/* Open every event device that has the A key. */
static void open_keyboards(void)
{
	unsigned long bits[(KEY_MAX + LONG_BITS) / LONG_BITS];
	struct dirent *de;
	DIR *dir;

	close_keyboards();
	dir = opendir("/dev/input");
	if (!dir)
		return;
	while ((de = readdir(dir)) && kbd_count < MAX_KEYBOARDS) {
		char path[300];
		int fd;

		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
		if (fd < 0)
			continue;
		memset(bits, 0, sizeof(bits));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 && test_bit(bits, KEY_A))
			kbd_fd[kbd_count++] = fd;
		else
			close(fd);
	}
	closedir(dir);
}

/*
 * True when every connected display is powered off, as when the desktop
 * blanks the screens. Atomic drivers keep the connector's dpms attribute in
 * step with its CRTC.
 */
static bool displays_off(void)
{
	int connected = 0, on = 0;
	glob_t g;

	if (glob("/sys/class/drm/card*-*/status", 0, NULL, &g) != 0)
		return false;
	for (size_t i = 0; i < g.gl_pathc; i++) {
		char buf[32], dpms[512];
		size_t len = strlen(g.gl_pathv[i]) - strlen("status");

		if (read_file(g.gl_pathv[i], buf, sizeof(buf)) || strncmp(buf, "connected", 9))
			continue;
		connected++;
		snprintf(dpms, sizeof(dpms), "%.*sdpms", (int)len, g.gl_pathv[i]);
		if (read_file(dpms, buf, sizeof(buf)) || !strncmp(buf, "On", 2))
			on++;
	}
	globfree(&g);
	return connected && !on;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* How far CLOCK_BOOTTIME is ahead of CLOCK_MONOTONIC: grows across a suspend. */
static double suspended_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9 - now_s();
}

/*
 * Wait ms, noting the time of the last key press in *last_key. Only the
 * timing of key events is used. With wake set, return at the first one.
 */
static void wait_ms(int ms, double *last_key, bool wake)
{
	double end = now_s() + ms / 1000.0;
	struct pollfd p[MAX_KEYBOARDS];

	if (!kbd_count) {
		sleep_ms(ms);
		return;
	}
	while (!stop) {
		int left = (int)ceil((end - now_s()) * 1000);
		bool key = false;

		if (left <= 0)
			return;
		for (int i = 0; i < kbd_count; i++)
			p[i] = (struct pollfd){ .fd = kbd_fd[i], .events = POLLIN };
		if (poll(p, kbd_count, left) <= 0)
			return;
		for (int i = kbd_count - 1; i >= 0; i--) {
			struct input_event ev[64];
			ssize_t len;

			if (p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				close(kbd_fd[i]);	/* unplugged */
				kbd_fd[i] = kbd_fd[--kbd_count];
				continue;
			}
			if (!(p[i].revents & POLLIN))
				continue;
			while ((len = read(kbd_fd[i], ev, sizeof(ev))) > 0)
				for (size_t j = 0; j < (size_t)len / sizeof(ev[0]); j++)
					if (ev[j].type == EV_KEY && ev[j].value)
						key = true;
		}
		if (key) {
			*last_key = now_s();
			if (wake)
				return;
		}
	}
}

/* Automatic level for a smoothed log10(lux), with on/off hysteresis. */
static long auto_level(const struct config *c, double log_lux, bool lit, long max)
{
	double lo = log10(c->full_lux), hi = log10(c->on_lux), frac, pct;

	if (log_lux >= log10(lit ? c->off_lux : c->on_lux))
		return 0;
	frac = (hi - log_lux) / (hi - lo);
	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;
	pct = c->min_pct + (c->max_pct - c->min_pct) * frac;
	return lround(max * pct / 100.0);
}

struct settle {
	bool active;		/* readings watched, not used yet */
	double max_log_lux;	/* highest log10(lux) seen since waking */
	double since;		/* monotonic time of the last rise */
	double started;		/* monotonic time the lid opened or display woke */
};

static void settle_reset(struct settle *s, double t)
{
	s->active = true;
	s->max_log_lux = NAN;
	s->since = t;
	s->started = t;
}

/*
 * Watch the sensor until it stops rising. Only upward steps reset the
 * window, so settling ends even if room light flickers.
 */
static bool settle_update(struct settle *s, double log_lux, double t)
{
	if (!s->active)
		return true;
	if (isnan(s->max_log_lux) || log_lux > s->max_log_lux + log10(SETTLE_RISE)) {
		s->max_log_lux = log_lux;
		s->since = t;
		return false;
	}
	if (t - s->since < SETTLE_STEADY_MS / 1000.0)
		return false;
	s->active = false;
	return true;
}

int main(int argc, char **argv)
{
	struct config c = {
		.full_lux = env_double("AURORA_KBD_LIGHT_FULL_LUX", 2),
		.on_lux = env_double("AURORA_KBD_LIGHT_ON_LUX", 60),
		.off_lux = env_double("AURORA_KBD_LIGHT_OFF_LUX", 120),
		.min_pct = (int)env_double("AURORA_KBD_LIGHT_MIN_PERCENT", 10),
		.max_pct = (int)env_double("AURORA_KBD_LIGHT_MAX_PERCENT", 50),
		.idle_s = env_double("AURORA_KBD_LIGHT_IDLE_SECONDS", DEFAULT_IDLE_S),
		.idle_pct = (int)env_double("AURORA_KBD_LIGHT_IDLE_PERCENT", 0),
	};
	char *led_dir, *als_path, bright_path[512], max_path[512];
	double log_lux = NAN, last_t, hold_log_lux = 0;
	long max, cur, written = -1, auto_last = -1, hold_level = 0, target, idle_level;
	bool hold = false, off_latch = false, write_failed = false, was_dark = false;
	/* relight: lit when it went dark */
	bool relight = false;
	/* dimmed: capped at the idle level now; dim_shown: that level is what was last shown */
	bool dimmed = false, dim_shown = false;
	struct settle settle = { 0 };
	double lid_retry_t, dark_since = -1, last_key, kbd_scan_t, suspended;
	int lid_fd;

	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			c.verbose = true;
	if (c.max_pct > 100)
		c.max_pct = 100;
	if (c.min_pct > c.max_pct)
		c.min_pct = c.max_pct;
	if (c.idle_pct > 100)
		c.idle_pct = 100;
	if (!(c.full_lux > 0 && c.full_lux < c.on_lux && c.on_lux <= c.off_lux)) {
		fprintf(stderr, "invalid lux thresholds\n");
		return 2;
	}

	led_dir = find_led();
	als_path = find_als();
	if (!led_dir || !als_path) {
		fprintf(stderr, "no %s; nothing to do\n",
			led_dir ? "ambient light sensor" : "keyboard backlight");
		return 0;
	}
	snprintf(bright_path, sizeof(bright_path), "%s/brightness", led_dir);
	snprintf(max_path, sizeof(max_path), "%s/max_brightness", led_dir);
	if (read_int(max_path, &max) || max <= 0) {
		fprintf(stderr, "cannot read %s\n", max_path);
		return 1;
	}
	lid_fd = open_lid();
	idle_level = lround(max * c.idle_pct / 100.0);
	if (c.idle_s > 0)
		open_keyboards();
	fprintf(stderr, "keyboard backlight %s (max %ld), light sensor %s, lid switch %s\n",
		led_dir, max, als_path, lid_fd >= 0 ? "found" : "not found");
	if (c.idle_s > 0)
		fprintf(stderr, "dims to %d%% after %g s without a key press (%d keyboards)\n",
			c.idle_pct, c.idle_s, kbd_count);

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	last_t = lid_retry_t = kbd_scan_t = last_key = now_s();
	suspended = suspended_s();

	while (!stop) {
		double lux, t = now_s(), dt = t - last_t, s = suspended_s();
		bool fading, dark, automatic, idle, resumed = s - suspended > 2;

		last_t = t;
		suspended = s;
		/* The lid switch can register after this starts. */
		if (lid_fd < 0 && t - lid_retry_t > 30) {
			lid_retry_t = t;
			lid_fd = open_lid();
		}
		if (c.idle_s > 0 && t - kbd_scan_t > RESCAN_S) {
			/* Keyboards come and go. */
			kbd_scan_t = t;
			open_keyboards();
		}
		/* Waking from suspend counts as a key press and may have missed the lid close. */
		if (resumed) {
			last_key = t;
			settle_reset(&settle, t);
		}
		if (read_int(bright_path, &cur)) {
			wait_ms(TICK_MS, &last_key, false);
			continue;
		}

		/* Someone else set the level: respect it. */
		if (written >= 0 && cur != written) {
			if (cur == 0) {
				off_latch = true;
				hold = relight = false;
			} else {
				/* A slider adjustment also counts as activity. */
				last_key = t;
				off_latch = false;
				if (cur == auto_last || (dim_shown && cur == idle_level)) {
					hold = false;	/* restored after desktop idle dimming */
				} else {
					hold = true;
					hold_level = cur;
					hold_log_lux = settle.active ? NAN : log_lux;
				}
			}
			if (c.verbose)
				fprintf(stderr, "external change to %ld: %s\n", cur,
					off_latch ? "off until turned on" :
					hold ? "holding" : "resuming");
			written = cur;
		}

		if (written < 0)
			written = cur;	/* first pass: start from what is set */

		if (!displays_off())
			dark_since = -1;
		else if (dark_since < 0)
			dark_since = t;
		dark = lid_closed(lid_fd) ||
		       (dark_since >= 0 && t - dark_since >= DISPLAYS_OFF_S);
		if (dark && !was_dark)
			relight = relight || written > 0;
		if (dark) {
			settle_reset(&settle, t);
		} else if (was_dark) {
			/* Opening the lid or waking a display counts as a key press. */
			last_key = t;
			settle.started = t;
		}
		was_dark = dark;

		/*
		 * The sensor sits in the lid: closed, it reads about 0 lux, and it
		 * takes a few seconds to climb back to the room's light after the lid
		 * opens. Watch it until it stops rising and keep the last reading
		 * meanwhile, so neither ends a held level or shows a dark-room level
		 * for a moment.
		 */
		if (!dark && read_lux(als_path, &lux) == 0) {
			/* Below full_lux the automatic level is flat; clamp 0/1 lux rounding. */
			double l = log10(fmax(lux, c.full_lux));
			bool was_settling = settle.active;

			if (settle_update(&settle, l, t)) {
				if (was_settling && c.verbose)
					fprintf(stderr,
						"light sensor steady at %.1f lux, %.1f s after waking\n",
						lux, t - settle.started);
				if (hold && isnan(hold_log_lux))
					log_lux = hold_log_lux = l;
				else
					log_lux = isnan(log_lux) ? l :
						  log_lux + (l - log_lux) * (dt / (SMOOTH_S + dt));
			}
		}
		if (hold && !settle.active && !isnan(log_lux) && !isnan(hold_log_lux) &&
		    fabs(log_lux - hold_log_lux) > log10(HOLD_RATIO))
			hold = false;

		idle = kbd_count > 0 && t - last_key >= c.idle_s;

		/* A closed lid or sleeping displays win; a held level comes back after. */
		automatic = false;
		if (dark || off_latch) {
			target = 0;
		} else if (hold) {
			target = hold_level;
			relight = false;
		} else if (!isnan(log_lux)) {
			/* Dimmed for idle or dark for a while, it was lit: keep the off threshold. */
			target = auto_level(&c, log_lux, written > 0 || dimmed || relight, max);
			automatic = true;
			/* Preserve wake hysteresis across a failed or idle-suppressed write. */
			if (target == 0 || written > 0)
				relight = false;
		} else {
			wait_ms(TICK_MS, &last_key, false);
			continue;
		}
		/* No key press for a while: no brighter than the idle level. */
		dimmed = idle && target > idle_level;
		if (dimmed) {
			target = idle_level;
			automatic = false;
		}
		if (labs(target - written) < DEADBAND && target != 0 && written != 0)
			target = written;
		fading = target != written;
		if (fading) {
			long step = max * FADE_TICK_MS / FADE_MS;
			long next;

			if (step < 1)
				step = 1;
			next = target > written ? written + step : written - step;
			if ((target > written && next > target) || (target < written && next < target))
				next = target;
			if (write_int(bright_path, next) == 0) {
				written = next;
				if (written > 0)
					relight = false;
				if (written == target && automatic)
					auto_last = target;
				write_failed = false;
			} else {
				if (!write_failed)
					fprintf(stderr, "cannot write %s: %s\n", bright_path,
						strerror(errno));
				write_failed = true;
				fading = false;	/* retry at the normal pace */
			}
			if (c.verbose && written == target) {
				if (automatic)
					fprintf(stderr, "lux %.2f -> level %ld\n", pow(10, log_lux), target);
				else
					fprintf(stderr, "%s -> level %ld\n", dark ?
						"lid closed or displays asleep" :
						dimmed ? "no key press" : "held level", target);
			}
		} else if (automatic) {
			auto_last = written;
		}
		if (written == target) {
			if (dimmed)
				dim_shown = true;
			else if (automatic || hold)
				dim_shown = false;
		}
		/* While dimmed, come back at the first key press. */
		wait_ms(fading ? FADE_TICK_MS : TICK_MS, &last_key, dimmed);
	}

	if (lid_fd >= 0)
		close(lid_fd);
	close_keyboards();
	return 0;
}
