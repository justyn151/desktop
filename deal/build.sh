#!/bin/sh
set -eu

cd -- "$(dirname -- "$0")"
action=build
case "${1:-}" in
    install) action=install; shift ;;
    uninstall) shift; exec ./install.sh uninstall "$@" ;;
esac
if ! command -v pkg-config >/dev/null 2>&1; then
    printf '%s\n' 'deal: pkg-config is required to find Lua 5.4, FreeType, and Fontconfig' >&2
    exit 1
fi

lua_package=
for package in lua5.4 lua-5.4 lua54 lua; do
    version=$(pkg-config --modversion "$package" 2>/dev/null) || continue
    case "$version" in
        5.4|5.4.*) lua_package=$package; break ;;
    esac
done
if [ -z "$lua_package" ]; then
    printf '%s\n' 'deal: Lua 5.4 development files are required (lua54-devel on openSUSE)' >&2
    exit 1
fi

if ! pkg-config --exists freetype2 fontconfig; then
    printf '%s\n' 'deal: FreeType and Fontconfig development files are required (freetype2-devel and fontconfig-devel on openSUSE)' >&2
    exit 1
fi

native_cflags=$(pkg-config --cflags "$lua_package" freetype2 fontconfig)
native_libs=$(pkg-config --libs "$lua_package" freetype2 fontconfig)
"${CC:-cc}" -std=c11 -Wall -Wextra -Wpedantic -O2 \
    "$@" $native_cflags terminal.c -o deal -lX11 -lGL -lutil $native_libs
if [ "$action" = install ]; then
    exec ./install.sh install
fi
