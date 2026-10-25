#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/Build"
WORK_DIR="$BUILD_DIR/appimage"
TOOLS_DIR="$WORK_DIR/tools"
ARCH=x86_64

for tool in curl python3 sha256sum pkg-config glib-compile-schemas; do
    command -v "$tool" >/dev/null || { echo "Missing packaging tool: $tool" >&2; exit 1; }
done

"$SCRIPT_DIR/build.sh" "$@"
mkdir -p "$TOOLS_DIR" "$WORK_DIR/tmp"
export TMPDIR="$WORK_DIR/tmp"

# Cache official release assets, checking GitHub's SHA-256 digests.
download_asset() {
    local repository="$1" filename="$2"
    local target="$TOOLS_DIR/$filename"
    local -a asset
    if ! (cd "$TOOLS_DIR" && sha256sum --check --status "$filename.sha256" 2>/dev/null); then
        curl --fail --location --retry 3 --silent --show-error \
            "https://api.github.com/repos/$repository/releases/tags/continuous" \
            -o "$TOOLS_DIR/release.json"
        readarray -t asset < <(python3 - "$TOOLS_DIR/release.json" "$filename" <<'PY'
import json, sys
release = json.load(open(sys.argv[1]))
asset = next(a for a in release['assets'] if a['name'] == sys.argv[2])
digest = asset['digest']
if not digest or not digest.startswith('sha256:'):
    raise SystemExit('The release has no SHA-256 digest')
print(asset['browser_download_url'])
print(digest.removeprefix('sha256:'))
PY
        )
        [[ ${#asset[@]} -eq 2 ]] || { echo "Could not find release metadata for $filename" >&2; exit 1; }
        curl --fail --location --retry 3 --silent --show-error "${asset[0]}" -o "$target.part"
        printf '%s  %s\n' "${asset[1]}" "$target.part" | sha256sum --check
        mv -- "$target.part" "$target"
        printf '%s  %s\n' "${asset[1]}" "$filename" > "$target.sha256"
    fi
}
if [[ -z "${LINUXDEPLOY:-}" ]]; then
    download_asset linuxdeploy/linuxdeploy "linuxdeploy-$ARCH.AppImage"
    LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-$ARCH.AppImage"
    chmod +x "$LINUXDEPLOY"
fi
LINUXDEPLOY="$(realpath "$LINUXDEPLOY")"
download_asset AppImage/type2-runtime "runtime-$ARCH"
export LDAI_RUNTIME_FILE="$TOOLS_DIR/runtime-$ARCH"

APPDIR="$(mktemp -d "$WORK_DIR/AppDir.XXXXXX")"
trap 'rm -rf -- "$APPDIR"' EXIT
mkdir -p "$APPDIR/usr/lib/gio/modules" "$APPDIR/usr/share/glib-2.0/schemas" \
    "$APPDIR/usr/share/doc/relapsegtk"
cp "$SCRIPT_DIR/LICENSE" "$APPDIR/usr/share/doc/relapsegtk/LICENSE"

gtk_prefix="$(pkg-config --variable=prefix gtk4)"
gio_libdir="$(pkg-config --variable=libdir gio-2.0)"
schema_dir="$(pkg-config --variable=schemasdir gio-2.0)"
pixbuf_modules="$(pkg-config --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0)"
pixbuf_query="$(pkg-config --variable=gdk_pixbuf_query_loaders gdk-pixbuf-2.0)"
svg_loader="$pixbuf_modules/libpixbufloader_svg.so"
[[ -f "$svg_loader" ]] || svg_loader="$pixbuf_modules/libpixbufloader-svg.so"
[[ -f "$svg_loader" && -x "$pixbuf_query" ]] || {
    echo 'Install the librsvg GdkPixbuf loader and gdk-pixbuf-query-loaders to package SVG icons.' >&2
    exit 1
}

# GTK 4 has no linuxdeploy GTK plugin; stage its runtime data explicitly.
shopt -s nullglob
schemas=("$schema_dir"/org.gtk.gtk4.*.gschema.xml)
[[ ${#schemas[@]} -gt 0 ]] || { echo "GTK 4 schemas not found in $schema_dir" >&2; exit 1; }
cp "${schemas[@]}" "$APPDIR/usr/share/glib-2.0/schemas/"
glib-compile-schemas "$APPDIR/usr/share/glib-2.0/schemas"
for directory in gtk-4.0 gtksourceview-5 icons/Adwaita; do
    if [[ -d "$gtk_prefix/share/$directory" ]]; then
        mkdir -p "$APPDIR/usr/share/$(dirname "$directory")"
        cp -a "$gtk_prefix/share/$directory" "$APPDIR/usr/share/$directory"
    fi
done
# Hicolor also contains icons from unrelated installed applications. Only its
# theme definition is needed; linuxdeploy adds RelapseGtk's own icon below.
mkdir -p "$APPDIR/usr/share/icons/hicolor"
cp "$gtk_prefix/share/icons/hicolor/index.theme" "$APPDIR/usr/share/icons/hicolor/"
if [[ -f "$gio_libdir/gio/modules/libdconfsettings.so" ]]; then
    cp -L "$gio_libdir/gio/modules/libdconfsettings.so" "$APPDIR/usr/lib/gio/modules/"
fi

# Bundle the dependency closure, including libraries on linuxdeploy's default
# exclusion list. Only the C runtime and host graphics-driver stack stay external.
libraries=(--library "$svg_loader")
ldd "$BUILD_DIR/relapsegtk" "$svg_loader" "$pixbuf_query" \
    "$APPDIR/usr/lib/gio/modules/"*.so > "$WORK_DIR/dependencies.txt"
python3 - "$WORK_DIR/dependencies.txt" > "$WORK_DIR/libraries.txt" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
if 'not found' in text:
    raise SystemExit('An application dependency could not be resolved:\n' + text)
print('\n'.join(sorted(set(re.findall(r'=> (/[^\n]+?) \(0x', text)))))
PY
while IFS= read -r library; do
    case "$(basename "$library")" in
        libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libresolv.so.*|ld-linux*.so.*|\
        libGL.so.*|libGLX.so.*|libGLdispatch.so.*|libEGL.so.*|libdrm*.so.*|libgbm.so.*) continue ;;
    esac
    libraries+=(--library "$library")
done < "$WORK_DIR/libraries.txt"
export APPIMAGE_EXTRACT_AND_RUN=1
"$LINUXDEPLOY" --appdir "$APPDIR" \
    --executable "$BUILD_DIR/relapsegtk" --executable "$pixbuf_query" \
    "${libraries[@]}" --deploy-deps-only "$APPDIR/usr/lib/gio/modules" \
    --desktop-file "$SCRIPT_DIR/packaging/appimage/org.relapse.RelapseGtk.desktop" \
    --icon-file "$SCRIPT_DIR/resources/icons/relapse.svg" --icon-filename org.relapse.RelapseGtk \
    --custom-apprun "$SCRIPT_DIR/packaging/appimage/AppRun"

export LDAI_OUTPUT="$BUILD_DIR/RelapseGtk-$ARCH.AppImage" ARCH
"$LINUXDEPLOY" --appdir "$APPDIR" --output appimage
printf '\n==> AppImage: %s\n' "$LDAI_OUTPUT"
