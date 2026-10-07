# Keybinds

The Settings window contains a Keybinds page with editable command sequences,
per-command reset, and Reset All. Clearing a sequence disables that keybind.
Duplicate sequences and overlapping sequence prefixes must be resolved before
settings can be accepted. Cancel discards changes.

Command identifiers, defaults, and repeat behavior are defined in
`src/gui/key_bindings.cpp`. Menu actions, toolbar actions, and keyboard-only
commands share the same action registry and router. The router yields to
text-entry widgets, modal dialogs, and popup menus. Keybinds match their full
modifier combinations and support multi-key sequences.

`SettingsDialog` owns override loading and saving in the application's
`keybinds` settings group. Accepted overrides are applied immediately and
restored at startup. Defaults are removed from the override store. Menu
shortcut labels and toolbar tooltips reflect the active bindings.
