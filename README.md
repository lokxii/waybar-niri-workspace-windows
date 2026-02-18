# Waybar niri workspace windows

wlr/taskbar, but shows only windows in the current focused workspace + sorted in horizontal order

Application icon searching logic is only partly adapted from wlr/taskbar in
Waybar source code. It is not a one-to-one rewrite from C++ to C. If this
module cannot find the icon for some apps, it may or may not be my fault.

## Building

Run `make`

## Waybar config

This is a cffi module. To use it in waybar, add `cffi/workspace-windows` to the
list of modules. Shared object file should be specified in config json. Icon
size can be set in config.

```json
"cffi/workspace-windows": {
    "module_path": "path/to/so/object",
    "icon_size": 14 // default icon size
}
```

## Styling

Styles are exposed as `workspace-windows`

```css
#workspace-windows button {
    background: none;
}
```

## Actions

No actions are supported right now because I do not need them

## Contribution

This module is written for myself, provided to you as SOURCE AVAILABLE. Patches
are welcomed. I may or may not respond to feature requests. Feel free to press
the fork button.
