# RelapseGtk

RelapseGtk is a Linux GTK script editor for Relapse. It connects to the local
Relapse script socket to submit Luau scripts, shows script output, and can
optionally expose script execution and output to local MCP clients.

## Build

Requirements: CMake 3.20 or newer, a C11 compiler, `pkg-config`, GTK 4.16 or
newer, GtkSourceView 5.16 or newer, GIO Unix, JSON-GLib 1.0, and libepoxy.

From this directory:

```sh
./build.sh
./run.sh
```

You can also build outside the source tree:

```sh
cmake -S . -B /tmp/relapsegtk-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/relapsegtk-build
/tmp/relapsegtk-build/relapsegtk
```

The app retries the local Unix socket at `/tmp/relapse.sock` until Relapse is
ready. It does not start or install Relapse.

## Local data

- Settings: `$XDG_CONFIG_HOME/relapsegtk/settings.ini` (by default
  `~/.config/relapsegtk/settings.ini`)
- Tab session: `$XDG_STATE_HOME/relapsegtk/session.json` (by default
  `~/.local/state/relapsegtk/session.json`)
- Script folder: `~/relapse-scripts-tmpdir`

The tab session can contain unsaved script text and file paths. The settings
file can contain an MCP bearer token. Do not add either file to a source
release.

## MCP server

The optional MCP server is disabled by default. When enabled, it listens on
`127.0.0.1` at the configured port. Set a bearer token before enabling its
`execute` or `logs` tools; clients send it as `Authorization: Bearer <token>`.
Any local process with that token can use the enabled tools.

## License

RelapseGtk is licensed under the GNU General Public License, version 3 or later
(`GPL-3.0-or-later`). You can use and modify it privately. If you distribute
the app or a modified version, you must license the distributed work under
the GPL and provide recipients with the corresponding source code. See
[LICENSE](LICENSE) for the full terms.
