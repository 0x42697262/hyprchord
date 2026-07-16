# hyprchords

sxhkd-style key chords (chains) for Hyprland, as a plugin. Press a sequence of
chords — e.g. `SUPER+X`, then `K` — and a dispatcher fires:

```
HOTKEY  := CHORD_1 ; CHORD_2 ; ... ; CHORD_n
CHORD_i := [MODIFIERS_i +] [~][@]KEYSYM_i
```

Under the hood each chord line is compiled into Hyprland submaps and binds, so
everything composes with normal Hyprland behavior (`hyprctl binds` shows the
generated binds, status bars get `submap` IPC events while a chain is pending).

## Configuration

```ini
plugin {
    hyprchords {
        timeout     = 3000    # ms a pending chain stays alive; 0 = no timeout
        abort_key   = Escape  # key that aborts a pending chain
        sticky_mods = 1       # modless steps also match with the previous step's mods held
        swallow     = 1       # 1: unmatched keys abort the chain and are swallowed
                              # 0: they pass to apps, chain stays pending (sxhkd behavior)

        chord = SUPER+X ; K , exec , kitty
        chord = SUPER+X ; F , exec , firefox
        chord = SUPER+A ; B ; C , exec , notify-send three-deep
        chord = SUPER+X ; ~@M , exec , notify-send release+passthrough

        # ':' = locked mode: stays at the chain tail, repeat the final key freely,
        # no timeout; only abort_key exits
        chord = SUPER+R : H , resizeactive , -40 0
        chord = SUPER+R : L , resizeactive ,  40 0

        # command-only {} groups cycle per press (sxhkd cycling)
        chord = SUPER+ALT+P , exec , hyprctl keyword cursor:no_warps {true,false}

        # mouse buttons as steps (sxhkd button1..3,8,9 or raw mouse:NNN)
        chord = SUPER+X ; button1 , exec , notify-send clicked

        # "any" matches every modifier state
        chord = any+XF86Calculator , exec , qalculate-gtk
    }
}
```

Syntax: `chord = STEP (;|:) STEP ... , dispatcher , arg`

### Sequence expansion (sxhkd `{...}` groups)

One `chord` line can expand into many, exactly like sxhkd:

```ini
# workspaces: SUPER+1..9,0 focuses, SUPER+SHIFT+1..9,0 moves the window
chord = SUPER+{_,SHIFT+}{1-9,0} , {workspace,movetoworkspace} , {1-9,10}

# directional resize
chord = SUPER+ALT+{J,L,K,semicolon} , resizeactive , {-20 0,0 20,0 -20,20 0}

# expansion works inside chains too
chord = SUPER+R ; {Q,R,W} , exec , notify-send rotate-{270,180,90}
```

- `{a,b,c}` in the key sequence expands the line once per element; the i-th
  group in the key sequence pairs element-wise with the i-th group after the
  comma. Multiple groups take the cartesian product.
- `_` is the empty element (`{_,SHIFT+}` = "without / with SHIFT").
- Single-character ranges expand: `{1-9,0}` = `1,2,...,9,0`, `{a-e}` etc.
- If the command part has no groups it's reused verbatim for every expansion.
- Groups **only in the command** create a cycling chord (see above).
- **Braces are always special** (as in sxhkd) — escape literal ones with `\`:
  `exec , awk '\{print $1\}'`. At most 512 expansions per line.

- Steps are separated by `;` (or `:` to engage locked mode from that step on).
  Within a step, modifiers and the key are joined with `+` (`SUPER+SHIFT+X`).
  `code:NN` is accepted for raw keycodes, `button1`/`button2`/`button3`/`button8`/
  `button9` or `mouse:NNN` for mouse buttons (scroll wheels can't be steps).
- Modifiers: everything Hyprland knows (`SUPER`, `SHIFT`, `CTRL`, `ALT`, `MOD1`-`MOD5`,
  `CAPS`, ...) plus sxhkd's `lock` (=CAPS), `hyper` (=MOD3), `mode_switch` (=MOD5),
  and `any` (matches every modifier state).
- `@` prefix on a key: trigger on release. `~` prefix: also pass the key event
  through to the focused app.
- Everything after the first `,` is `dispatcher , arg` — any Hyprland
  dispatcher works (`exec`, `togglefloating`, `workspace`, ...).
- Set the option values (`abort_key`, `swallow`, ...) **before** the `chord` lines.

Behavior, matching sxhkd:

- A pending chain times out after `timeout` ms and resets.
- Pressing the abort key resets the chain.
- With `swallow = 1` (default), any key that doesn't continue the chain aborts it
  and is swallowed (not delivered to the focused app); bare modifier presses don't
  abort. With `swallow = 0`, unmatched keys pass through and the chain stays
  pending — exactly how sxhkd's selective grabbing behaves.
- `:` instead of `;` engages **locked mode** once that step is reached: the chain
  stays resident at its tail (repeat the final key as often as you like), the
  timeout is disabled, unmatched keys never abort — only the abort key exits.
- **Cycling**: `{a,b,c}` groups only in the command make one chord whose command
  advances to the next variant on each press (multiple groups advance together
  and must be the same length).
- Chords sharing a prefix (`SUPER+X ; K` and `SUPER+X ; F`) share the pending
  state. One chord being a strict prefix of another is a config error.

### Migrating from sxhkd

Three ways, all sharing the same converter. Every hotkey becomes an `exec`
chord; commands are copied **verbatim** (`bspc`, `xrandr`, ... are not
translated to Hyprland dispatchers — replace those by hand). Suspicious
constructs (mismatched `{}` group sizes, empty group elements, trailing `;`)
are reported with their sxhkdrc line numbers.

**1. Source the sxhkdrc directly** — zero steps, the sxhkdrc stays the single
source of truth, re-read on every `hyprctl reload`:

```ini
plugin {
    hyprchords {
        sxhkd_source = ~/.config/sxhkd/sxhkdrc
    }
}
```

An unreadable file is a config error; individual bad hotkeys are skipped and
reported as notifications after the reload.

**2. One-shot import** — writes a `chord = ...` config file you can hand-edit
(e.g. to migrate bspc commands to real dispatchers incrementally):

```sh
hyprctl dispatch hyprchords_import "~/.config/sxhkd/sxhkdrc ~/.config/hypr/chords.conf"
# then in hyprland.conf:  source = ~/.config/hypr/chords.conf
```

**3. Offline script** — same conversion without the plugin loaded:

```sh
tools/sxhkd2hyprchords ~/.config/sxhkd/sxhkdrc -o ~/.config/hypr/chords.conf
```

Warnings go to stderr; `--no-wrap` emits bare `chord =` lines without the
`plugin { hyprchords { ... } }` block.

### Runtime control

- `hyprctl dispatch hyprchords_toggle ""` — disable/enable all chords (sxhkd's
  SIGUSR2 grab toggle). Also accepts `on`/`off`.
- `hyprctl dispatch hyprchords_abort key` — abort a pending chain from a script
  (sxhkd's SIGALRM).
- `hyprctl dispatch hyprchords_import "<sxhkdrc> <output.conf>"` — convert an
  sxhkd config to a hyprchords config file (see *Migrating from sxhkd*).

### Status events (sxhkd's status fifo)

Chain begin/end is visible as normal `submap>>hc:...` IPC events. Additionally
the plugin emits `hyprchords>>` events on socket2:

```
hyprchords>>fire,<chord>,<dispatcher>,<arg>   # a chord fired
hyprchords>>abort                             # pending chain aborted
hyprchords>>timeout                           # pending chain timed out
hyprchords>>lock,<submap>                     # locked mode engaged
hyprchords>>enabled / disabled                # hyprchords_toggle state
```

## Building

Requires Hyprland headers matching the **exact running commit** (plugins are
ABI-coupled; use the same compiler Hyprland was built with).

```sh
# against installed headers (hyprland.pc present):
make

# against a source checkout:
make HYPRLAND_HEADERS=~/Github/Hyprland
```

Load it:

```sh
hyprctl plugin load $(pwd)/hyprchords.so   # config reload is triggered automatically
hyprctl plugins list
```

Or with hyprpm, once pushed to a git remote:

```sh
hyprpm add <repo-url>
hyprpm enable hyprchords
hyprpm reload
```

## Testing

Watch chain state via the submap IPC event (submaps are named `hc:<prefix>`):

```sh
socat -u UNIX-CONNECT:$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock - | grep submap
```

- `SUPER+X` → `submap>>hc:super+x`; `K` within the timeout → dispatcher fires, `submap>>` resets.
- `SUPER+X` then wait past the timeout → auto reset.
- `SUPER+X` then an unbound key → chain aborts, key swallowed (with `swallow = 0`:
  key reaches the app, chain stays pending).
- `SUPER+R` then `H H H` (locked chord) → repeats, no timeout; `Escape` exits.
- A cycling chord → alternates its command on each press.
- `hyprctl dispatch hyprchords_toggle ""` → chords dead; again → alive.
- `socat ... | grep hyprchords` → `fire`/`abort`/`timeout`/`lock` events appear.
- `hyprctl reload` repeatedly → `hyprctl binds | grep -c hyprchords` stays stable.
- `hyprctl plugin unload .../hyprchords.so` → generated binds disappear cleanly.
