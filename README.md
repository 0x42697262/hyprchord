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

        chord = SUPER+X ; K , exec , kitty
        chord = SUPER+X ; F , exec , firefox
        chord = SUPER+A ; B ; C , exec , notify-send three-deep
        chord = SUPER+X ; ~@M , exec , notify-send release+passthrough
    }
}
```

Syntax: `chord = STEP ; STEP ; ... , dispatcher , arg`

- Steps are separated by `;`. Within a step, modifiers and the key are joined
  with `+` (`SUPER+SHIFT+X`). `code:NN` is accepted for raw keycodes.
- `@` prefix on a key: trigger on release. `~` prefix: also pass the key event
  through to the focused app.
- Everything after the first `,` is `dispatcher , arg` — any Hyprland
  dispatcher works (`exec`, `togglefloating`, `workspace`, ...).
- Set `abort_key` **before** the `chord` lines.

Behavior, matching sxhkd:

- A pending chain times out after `timeout` ms and resets.
- Pressing the abort key resets the chain.
- Pressing any key that doesn't continue the chain aborts it, and the key is
  swallowed (not delivered to the focused app). Bare modifier presses don't abort.
- Chords sharing a prefix (`SUPER+X ; K` and `SUPER+X ; F`) share the pending
  state. One chord being a strict prefix of another is a config error.

Not supported (yet): sxhkd's `:` locked modes, mouse buttons in chords.

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
- `SUPER+X` then an unbound key → chain aborts, key swallowed.
- `hyprctl reload` repeatedly → `hyprctl binds | grep -c hyprchords` stays stable.
- `hyprctl plugin unload .../hyprchords.so` → generated binds disappear cleanly.
