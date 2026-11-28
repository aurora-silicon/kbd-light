# kbd-light

`aurora-kbd-light` drives an Apple silicon MacBook's keyboard backlight from its ambient light sensor, the way macOS does.
`aurora-display-light` does the same for the built-in display's brightness ([below](#display-brightness)).

- **Follows the room:** the keyboard is at its brightest in the dark and fades out as the room gets brighter. Above `ON_LUX` it stays off; once lit, it stays on until `OFF_LUX`. Changes fade over about half a second.
- **Respects manual changes:** a change made by anyone else wins. Turning the backlight off (for example from the desktop's slider, or the desktop's idle dimming) keeps it off until it is turned on again. Any other level holds until the room's light changes about fourfold. When the desktop restores the last automatic level after idle dimming, automatic control resumes.
- **Lid and display sleep:** with the lid closed, or once every connected display has been powered off for a second (as when the desktop blanks the screens), the backlight is off. It fades back in when a display wakes, to the held level if one was set. The light sensor sits in the lid, so it is not read while the lid is closed or the displays sleep, nor, after the lid opens or a display wakes, until its reading stops rising: the level comes back as it was before.
- **Idle keyboard:** after 30 seconds without a key press, the backlight fades out. A key press, a brightness adjustment above zero, a resume from suspend, or opening the lid or waking a display starts the timer again. Only the timing of key events is used. Set `AURORA_KBD_LIGHT_IDLE_SECONDS=0` to let the desktop handle idle dimming.

It reads the light sensor and writes the LED through sysfs, and reads the lid switch (and, for the idle setting, the keyboards) through evdev, so it needs nothing beyond libc. It works with any desktop and runs as a system service. It finds the first LED named `*kbd_backlight` and the first IIO device with `in_illuminance_input` (`aop-sensors-als` on Apple silicon). Without either it exits quietly.

## Settings

Set these in `/etc/default/aurora-kbd-light`:

| Variable | Default | Meaning |
| --- | --- | --- |
| `AURORA_KBD_LIGHT_FULL_LUX` | 2 | At or below this, the brightest automatic level |
| `AURORA_KBD_LIGHT_ON_LUX` | 60 | Below this the backlight comes on |
| `AURORA_KBD_LIGHT_OFF_LUX` | 120 | Above this it goes off again |
| `AURORA_KBD_LIGHT_MIN_PERCENT` | 10 | Level just after it comes on |
| `AURORA_KBD_LIGHT_MAX_PERCENT` | 50 | Level in the dark |
| `AURORA_KBD_LIGHT_IDLE_SECONDS` | 30 | Dim after this many seconds without a key press (0: never) |
| `AURORA_KBD_LIGHT_IDLE_PERCENT` | 0 | Level while dimmed, if lower than the current one (0: off) |

`aurora-kbd-light -v` logs each level it sets and each change it respects.

## Display brightness

`aurora-display-light` sets the built-in display's brightness from the same light sensor. It runs in the user's
session and sets the brightness through the desktop, so the desktop's brightness keys, slider, on-screen indicator and
idle dimming keep working as before.

- **A curve per room:** the brightness follows a curve given at five light levels: a dark room (0 lx), a dim room
  (10 lx), indoor lighting (100 lx), a bright room (500 lx) and daylight (2000 lx). Between them it is interpolated
  on a log scale of light against a perceptual scale of brightness (logarithmic in nits, from the panel's lowest
  level to its highest). There are three built-in profiles, and a custom one:

  | Profile | 0 lx | 10 lx | 100 lx | 500 lx | 2000 lx |
  | --- | --- | --- | --- | --- | --- |
  | Dimmer | 12 | 30 | 100 | 210 | 380 |
  | Balanced (default) | 25 | 60 | 170 | 320 | 500 |
  | Brighter | 50 | 110 | 270 | 440 | 600 |

  Values are in nits and are capped to the panel's maximum.
- **Calm:** the screen follows the light only once the light has moved by a set fraction **and stayed there**:
  a passing shadow, a hand near the sensor, or someone leaning in does not move it. Light is smoothed over about half
  a second before it is compared. Changes then run as smooth transitions on the perceptual scale; dimming waits
  longer and moves more slowly than brightening. Three response settings:

  | Response | Brighten when the light rises by | Dim when it falls by | Wait (brighten / dim) | Transition (brighten / dim) |
  | --- | --- | --- | --- | --- |
  | Calm | 60 % | 70 % | 5 s / 15 s | 3 s / 10 s |
  | Balanced (default) | 35 % | 40 % | 3 s / 8 s | 2 s / 6 s |
  | Quick | 15 % | 20 % | 1 s / 3 s | 1 s / 3 s |

  A target within 5 % of the shown level is left alone.
- **Learns:** a brightness you choose (keys or slider) moves the curve to give that brightness at that light, from
  then on. Only the two points around the current light move, each by at most a quarter of the perceptual range per
  change; points further out move only as far as needed to keep the curve rising with the light. The light level a
  choice is learned at is the median of the last two seconds. What is learned is kept per profile in
  `~/.local/state/aurora-display-light/learned`; the settings page can forget it.
- **Desktops:**
  - **Plasma:** the brightness is set through PowerDevil (`org.kde.ScreenBrightness` on the session bus). A change
    from the keys, the slider or another program arrives with its source, so it is told apart from the service's own
    exactly; it is learned once the level has stayed put for a second. Plasma's idle dimming is applied on top of the
    level set here and is not a brightness change at all. PowerDevil's own levels (a power profile's on AC or battery,
    `profile_brightness`; idle dimming on versions before 6.8, `dimming`) are held, not learned; of its own changes
    only the keys (`brightness_key`) are. A change without a source (one PowerDevil only observed, such as the
    compositor's level changing underneath it, or one made through its older interface) is held too: nothing says a
    person made it. A held level stays until it returns to the automatic one or the light changes threefold.
    Plasma (6.6 and later) also has an automatic brightness of its own, in KWin (Display Configuration, "Automatic
    brightness"); while it is on for the built-in screen (`automaticBrightness` in `~/.config/kwinoutputconfig.json`),
    the service leaves the panel to it, since the two would fight. The settings page switches between them.
  - **GNOME** has automatic brightness of its own (Settings, Power); the service steps aside there.
  - **Others** (Hyprland, Sway, ...): the brightness is set through logind (`SetBrightness` on the user's session),
    or the sysfs file if that is writable. A change made by something else can't be told apart from idle dimming
    there, so it is held, not learned: the level stays until it returns to the automatic one or the light changes
    threefold.
  - For ten seconds after the service starts, the Mac wakes or the lid opens, any change is held, not learned: it may
    be a level the desktop restores.
- **Lid and display sleep:** with the lid closed (from logind) or every display off, the service leaves the panel
  alone, and starts over from the light when the display is back.

It works on every Apple silicon MacBook whose light sensor and panel Linux drives: it looks for the first IIO device
with `in_illuminance_input` and the first `platform` backlight whose parent is an Apple DCP (device tree compatible
`apple,dcp` or `apple,<soc>-dcp`), whose brightness the DCP driver reports in nits. Without either, or on a Mac without
a built-in display, it exits quietly.

### Settings

`~/.config/aurora-display-lightrc` (KConfig format; `/etc/xdg/aurora-display-lightrc` gives system-wide defaults). The
service re-reads it when it changes.

```ini
[General]
Enabled=true
Profile=balanced        # dimmer, balanced, brighter or custom
Response=balanced       # calm, balanced or quick
Learn=true

[Custom]                # nits for Profile=custom
DarkNits=25             # 0 lx
DimNits=60              # 10 lx
IndoorNits=170          # 100 lx
BrightNits=320          # 500 lx
DaylightNits=500        # 2000 lx

[Advanced]
MinNits=1               # the panel's lowest level: the floor of the perceptual scale
Deadband=0.05           # targets closer than this fraction to the shown level are left alone
Backlight=              # a /sys/class/backlight name, if not the Apple panel's
```

The service keeps its state in `$XDG_RUNTIME_DIR/aurora-display-light/status` (light, target, level) for settings pages.

**Plasma settings page:** `kcm/` builds a System Settings module, *Display & Monitor → Auto-Brightness*: which automatic
brightness drives the built-in screen (this one, Plasma's own, or none; choosing one turns the other off, Plasma's
through libkscreen), the
profile (and the custom curve's five points), the response, learning, forgetting what was learned, and a graph of the
curve with the room's light and the screen's level live.

```sh
cmake -B kcm/build -S kcm -DCMAKE_INSTALL_PREFIX=/usr
cmake --build kcm/build
DESTDIR=... cmake --install kcm/build
```

### Diagnostics

- `aurora-display-light -v` logs every transition and every change made by others (in the journal:
  `journalctl --user -u aurora-display-light`).
- `aurora-display-light --trace FILE` writes each light sample with the target, the level set, the level the desktop
  reports and the backlight's own sysfs level.
- `aurora-display-light --replay FILE [--profile NAME] [--response NAME]` runs the model over a recorded trace (CSV of
  `t,lux[,picked nits]`) and prints what it would do. `make check` replays the light sequences in `display-light/fixtures` (rebuilt from J815 logs).

## Build

```sh
make
make check
make install DESTDIR=... PREFIX=/usr
```

`aurora-kbd-light` needs only libc and runs as a system service, `aurora-kbd-light.service`. `aurora-display-light`
needs libsystemd (sd-bus) and runs as a user service, `aurora-display-light.service`, started with the graphical
session (`systemctl --user enable aurora-display-light.service`).
