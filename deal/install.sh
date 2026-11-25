#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"

action=${1:-install}
if [ "$#" -gt 1 ]; then printf '%s\n' 'Usage: install.sh [install|uninstall]' >&2; exit 2; fi
prefix=${PREFIX:-${HOME:?HOME or PREFIX is required}/.local}
dest=${DESTDIR:-}
case "$prefix" in /*) ;; *) printf '%s\n' 'deal: PREFIX must be absolute' >&2; exit 1 ;; esac
case "$dest" in ''|/*) ;; *) printf '%s\n' 'deal: DESTDIR must be absolute' >&2; exit 1 ;; esac
case "$prefix" in
    *'
'*) printf '%s\n' 'deal: PREFIX must fit on one line' >&2; exit 1 ;;
esac
if printf '%s' "$prefix" | LC_ALL=C grep -q '[[:cntrl:]=]'; then
    printf '%s\n' 'deal: PREFIX contains a character unsupported in desktop launchers' >&2
    exit 1
fi
root=$dest$prefix
manifest=$root/share/deal/.install-manifest
paths='bin/deal share/applications/deal.desktop share/xfce4/helpers/deal.desktop share/deal/init.example.lua share/deal/deal.terminfo share/terminfo/d/deal share/terminfo/64/deal'

checksum() {
    sha256sum < "$1" | cut -d ' ' -f 1
}

owned() {
    [ -f "$manifest" ] && [ ! -L "$manifest" ] &&
        [ -f "$root/$1" ] && [ ! -L "$root/$1" ] &&
        grep -Fxq "$(checksum "$root/$1") $1" "$manifest"
}

case "$action" in
    uninstall)
        if [ ! -f "$manifest" ] || [ -L "$manifest" ] ||
            ! grep -Fxq 'deal installation v1' "$manifest"; then
            printf '%s\n' "deal: no installation recorded under $root" >&2
            exit 1
        fi
        for relative in $paths; do
            if owned "$relative"; then
                rm -- "$root/$relative"
            elif [ -e "$root/$relative" ] || [ -L "$root/$relative" ]; then
                printf '%s\n' "deal: kept modified or untracked file $root/$relative" >&2
            fi
        done
        rm -- "$manifest"
        rmdir -- "$root/share/deal" 2>/dev/null || true
        exit 0 ;;
    install) ;;
    *) printf '%s\n' 'Usage: install.sh [install|uninstall]' >&2; exit 2 ;;
esac

stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
trap 'exit 1' HUP INT TERM
install -Dm755 deal "$stage/bin/deal"
install -Dm644 init.example.lua "$stage/share/deal/init.example.lua"
install -Dm644 deal.terminfo "$stage/share/deal/deal.terminfo"
mkdir -p -- "$stage/share/applications" "$stage/share/xfce4/helpers"
executable=$prefix/bin/deal
# Desktop Entry string escaping precedes Exec quoting.
exec_path=$(printf '%s' "$executable" | sed -e 's/\\/\\\\\\\\/g' -e 's/["`$]/\\\\&/g' -e 's/%/%%/g')
try_path=$(printf '%s' "$executable" | sed 's/\\/\\\\/g')
{
    sed '/^Exec=/d; /^TryExec=/d' deal.desktop
    printf 'Exec="%s"\nTryExec=%s\n' "$exec_path" "$try_path"
} > "$stage/share/applications/deal.desktop"
{
    sed '/^X-XFCE-Binaries=/d' deal-xfce.desktop
    printf 'X-XFCE-Binaries=%s;\n' "$(printf '%s' "$try_path" | sed 's/;/\\;/g')"
} > "$stage/share/xfce4/helpers/deal.desktop"
if command -v tic >/dev/null 2>&1; then
    mkdir -p -- "$stage/share/terminfo"
    tic -x -o "$stage/share/terminfo" deal.terminfo
else
    printf '%s\n' 'deal: tic unavailable, installed terminfo source. xterm-color remains available.' >&2
fi

if [ -e "$manifest" ] || [ -L "$manifest" ]; then
    if [ -L "$manifest" ] || ! grep -Fxq 'deal installation v1' "$manifest"; then
        printf '%s\n' "deal: refusing to replace $manifest" >&2
        exit 1
    fi
fi
for relative in $paths; do
    if [ -f "$stage/$relative" ] && { [ -e "$root/$relative" ] || [ -L "$root/$relative" ]; } &&
        ! owned "$relative"; then
        printf '%s\n' "deal: refusing to replace $root/$relative" >&2
        exit 1
    fi
done
{
    printf '%s\n' 'deal installation v1'
    for relative in $paths; do
        if [ -f "$stage/$relative" ]; then
            case "$relative" in bin/*) mode=755 ;; *) mode=644 ;; esac
            install -Dm"$mode" -- "$stage/$relative" "$root/$relative"
            printf '%s %s\n' "$(checksum "$root/$relative")" "$relative"
        elif owned "$relative"; then
            printf '%s %s\n' "$(checksum "$root/$relative")" "$relative"
        fi
    done
} > "$stage/manifest"
install -Dm644 -- "$stage/manifest" "$manifest"
printf '%s\n' "Installed deal under $root"
