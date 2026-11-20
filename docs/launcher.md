# Title launcher

A small window next to the game's executable: the player picks the main settings, presses Play, and the game starts.
It edits the title's own `settings/hardware.toml` and `settings/mapping.toml` in place (only the changed lines
differ; comments, order and every other line stay), so the game started without the launcher has the same settings.

One launcher for every title: the code is the SDK's (`src/launcher/`), what it shows comes from two files staged
next to the executable:

- `launcher/common.toml` (SDK): the settings every title has (display, renderer, upscaling, controls, language, log).
- `launcher/launcher.toml` (the title's): its name, colours, fonts and wordmark, and its own settings.

It runs on the SDK's window, presenter (Direct3D 12 on Windows, Vulkan on Linux) and Dear ImGui drawer from
`rexruntime`, the ones the in-game overlays use: no other library, and the same code on Windows and Linux.

## Adding it to a title

In the title's `CMakeLists.txt`, after `rexglue_setup_target`:

```cmake
rexglue_add_launcher(mytitle ICON_RC src/mytitle.rc)
```

This builds `mytitle_launcher(.exe)` next to `mytitle(.exe)` and copies `launcher/` there (the SDK's
`common.toml`, the default font as `fonts/default.ttf`, and everything in the title's `launcher/` folder). Building
the game builds the launcher. `ICON_RC` (Windows) gives it the game's icon. A release ships the launcher's
executable and its `launcher/` folder next to the game.

The launcher finds `settings/` and `assets/` as the games do: next to itself in a release, up the folders in a build
tree (the repository root).

## What Play does

1. Writes the changed settings (as Save).
2. Starts the game from its folder. Play is off while a file of `required_files` or the game's executable is
   missing (the status line says which).
3. Closes its window and frees its GPU device, and waits for the game. Waiting, not exiting, keeps Steam's tracking
   of the shortcut (playtime, overlay, Steam Input) on the launcher's process tree.
4. When the game exits normally, the launcher exits. When it exits with an error code (a crash), the launcher opens
   again and says so, with Safe mode and the logs one click away.

**Safe mode** starts the game once with the `safe_mode` flags (`common.toml`: the emulated renderer on Vulkan, scale
1, no upscaler, no anti-aliasing); the settings files don't change.

## launcher.toml

```toml
[game]
name = "My Title"                       # window title, wordmark fallback
required_files = ["assets/default.xex"] # relative to the game folder; Play needs them
# safe_mode = ["--flag=value", ...]     # replaces common.toml's

[common]
hide = ["max_local_users"]              # common rows this title doesn't use

[theme]
bg = "#13160F"          # window; everything not given is derived from bg, text and accent
bg2 = "#0B0C09"         # bottom of the background gradient
side = "#0F110C"        # side bar, help panel, footer
panel = "#20251A"       # controls
panel_hot = "#2A3022"   # hover, the selected tab
border = "#323A27"
text = "#ECE8DC"
dim = "#A3A693"         # secondary text
accent = "#D9822B"      # Play, chosen segments, switches
accent_hi = "#E89A4A"   # top of the Play gradient, hover
accent_text = "#1A1208" # text on the accent
accent2 = "#A9B87A"     # keyboard focus ring, the setting's key in the help panel
warn = "#E9A35A"
ok = "#8FBF5A"
display_font = "fonts/Oswald-SemiBold.ttf"  # titles, Play, wordmark; relative to launcher/
# body_font = "fonts/...ttf"                # default: fonts/default.ttf (Roboto Medium)
tagline = "Recompiled"                      # small line under the wordmark

[[theme.wordmark]]       # one table per line of the logo text, top to bottom
text = "TWO"
size = 66                # px
tracking = 0.03          # letter spacing, em
indent = 0               # px from the side bar's left padding
shear = 0                # degrees, leaning right (italic look)
scale_x = 1.0            # horizontal stretch
rotate = 0               # degrees counter-clockwise, the line with its plate
font = "display"         # "display", "body", or a file: "fonts/Exo2-ExtraBoldItalic.ttf"
same_line = false        # continue the previous line, bottoms aligned
color = "#ECE8DC"                                  # or:
gradient = ["#F4F4F0", "#C3C4BD"]                  # top to bottom, 2 or more stops
outline = { color = "#23251F", width = 1 }
shadow = { color = "#090A07", x = 2, y = 3 }
plate = { color = "#A3140E", pad_x = 20, pad_y = 6 } # a band behind the text
texture = { kind = "concrete", color = "#55574F", amount = 0.85, scale = 10 }
# texture: a pattern over the letters only (the launcher builds it from the letters' shapes when it
# starts), in `color`: "concrete" (mottled stone, a darker grey) or "distress" (worn paint: blotches and
# specks in the colour behind the text, e.g. the plate's). amount 0..1, scale = patch size in px.

[[row]]                  # the title's own settings (and replacements of common ones, same key)
page = "Game"            # one of common.toml's pages: Display, Graphics, Controls, Game, Advanced
key = "mytitle_fps_unlock"
file = "hardware"        # settings/<file>.toml (hardware or mapping)
kind = "toggle"          # toggle, choice, select, slider, button
label = "Unlock frame rate"
help = "Shown in the help panel while the row is hovered or focused."
default = true           # what "Reset all settings" puts back
restart = true           # "Restart needed" in the help panel
```

Row kinds:

| kind | fields | writes |
|---|---|---|
| `toggle` | `invert = true`: on while the setting is false (`*_disable_*` settings) | `true` / `false` |
| `choice` | `choices = [[value, "Label"], ...]`: a row of segments (few, short choices) | the value as typed: `true`, `2`, `"fsr"` |
| `select` | same `choices`: a list that opens (many or long choices) | as above |
| `slider` | `min`, `max`, `step`, `decimals`, `suffix = "°"`, `integer = true` | a number |
| `button` | `button = "Text"`, `action = "bindings"`, `"open_settings"`, `"open_logs"`, `"reset_page"`, `"reset_all"` | nothing |
| `keybind` | a key binding of the title (`bind_*`): `default = "V"`; shown under PC actions on the key bindings screen (no `page` needed) | `"V,F5"` |

`choice` and `select` rows may take their choices from the system instead of `choices`: `source = "displays"`
(the monitors, numbered as the `monitor` setting counts them: 0 Default, 1 the first) and `source =
"display_modes"` (the full-screen sizes of the monitor chosen in the `monitor` row, written as `"1920x1080"`). A
value in the file that the list doesn't have stays as a choice of its own.

## Key bindings

Controls > Edit bindings opens a picture of an Xbox 360 style pad (drawn by the launcher, no artwork) with a box per
button showing its keys. Pick a button, then **+ Add** and press a key, a combination (Shift, Ctrl, Alt + a key),
a mouse button or the wheel; × removes a key, Esc cancels, Delete clears the row. A key bound to two buttons is
marked. These are the 25 `keybind_*` settings of the mouse and keyboard driver (defaults: the driver's own), in
`settings/mapping.toml`; the title's own `keybind` rows are under PC actions. Gamepad buttons themselves are not
remapped (the SDK has no such setting).

A choice may have a third element that hides it: `"windows"`, `"linux"`, or `"file:rexgpu-native"` (shown only when
that plugin is next to the game).

Conditions, on any row:

- `enable_if = { key = "mytitle_fps_unlock", value = true }`: greyed out unless the other setting has that value.
- `lock_if = { key = "gpu_plugin", value = "native", shows = "vulkan" }`: while it holds, the row shows `shows`
  and can't be changed; the setting's own value stays in the file (common.toml's Graphics API row: Native renders
  with Vulkan only).

A setting missing from the settings file shows the row's `default` (or nothing chosen) and is written only when the
player changes it.

## Fonts

Dear ImGui draws TrueType fonts at fixed sizes: every size a launcher uses is rasterized when it starts (at the
monitor's scale, so text stays sharp at 150 % and 200 %), with Latin, Cyrillic and a few symbols (× ° · … — –).
Use static fonts (a `static/` file from a Google Fonts download), not variable ones: the rasterizer reads only a
variable font's default instance. Ship a font's licence with it (`fonts/OFL.txt` for SIL Open Font License fonts).
A missing font falls back to the default one.

## Not there yet

A background picture, glow on the wordmark, the launcher's own settings (start the game directly, come back after
playing), the Windows 11 title bar in the theme's colour. See the repository's memory/33-launcher.md.
