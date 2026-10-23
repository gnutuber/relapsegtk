# RelapseGtk

RelapseGtk is a Linux GTK script editor for Relapse. It connects to the local
Relapse script socket to submit Luau scripts, shows script output, and can
optionally expose script execution.

## Build

Requirements: CMake 3.20 or newer, a C11 compiler, `pkg-config`, GTK 4.16 or
newer, GtkSourceView 5.16 or newer, GIO Unix, JSON-GLib 1.0, and libepoxy.

From this directory:

```sh
./build.sh
./run.sh
```

To build a portable AppImage with GTK, GtkSourceView, and their dependencies:

```sh
./build-appimage.sh
```

The result is `Build/RelapseGtk-x86_64.AppImage`.
Packaging also needs `curl`, Python 3, `sha256sum`, `glib-compile-schemas`,
`gdk-pixbuf-query-loaders`, and the librsvg GdkPixbuf loader. The script downloads
and caches linuxdeploy and the AppImage runtime in `Build/appimage/tools` on its first run; set
`LINUXDEPLOY=/path/to/linuxdeploy.AppImage` to use an existing copy.
Like `build.sh`, it accepts CMake arguments and the `BUILD_TYPE` environment variable.

## Local data

- Settings: `$XDG_CONFIG_HOME/relapsegtk/settings.ini` (by default
  `~/.config/relapsegtk/settings.ini`)
- Tab session: `$XDG_STATE_HOME/relapsegtk/session.json` (by default
  `~/.local/state/relapsegtk/session.json`)
- Script folder: `~/relapse-scripts-tmpdir`

## License

RelapseGtk is licensed under the GNU General Public License, version 3 or later
(`GPL-3.0-or-later`). You can use and modify it privately. If you distribute
the app or a modified version, you must license the distributed work under
the GPL and provide recipients with the corresponding source code. See
[LICENSE](LICENSE) for the full terms.
