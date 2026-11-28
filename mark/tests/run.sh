#!/bin/sh
# Run by `make check`: golden outputs, exit codes, then the CommonMark spec
# examples and pathological inputs when python3 is installed.
# usage: tests/run.sh MARK MARK_TEST
mark=$1
mark_test=$2
dir=$(dirname "$0")
failed=0
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
unset NO_COLOR
# A config and palette of the tests' own, so the user's don't change the output.
home=$(mktemp -d)
trap 'rm -f "$tmp"; rm -rf "$home"' EXIT
export XDG_CONFIG_HOME="$home/config" XDG_CACHE_HOME="$home/cache"

fail() {
    echo "FAIL: $*"
    failed=1
}

# name.md renders to name.txt with --plain, and to name.ansi without it.
# A name ending in -N renders at width N, otherwise 80.
for input in "$dir"/*.md; do
    base=${input%.md}
    width=80
    case $base in *-[0-9]*) width=${base##*-} ;; esac
    if [ -f "$base.txt" ]; then
        "$mark" --plain --width "$width" "$input" > "$tmp" || fail "$input exited $?"
        cmp -s "$tmp" "$base.txt" || { fail "$input (plain)"; diff "$base.txt" "$tmp" | head -20; }
    fi
    if [ -f "$base.ansi" ]; then
        # Links to relative files depend on where the repository is, so
        # the styled tests use absolute URLs only.
        "$mark" --width "$width" "$input" > "$tmp" || fail "$input exited $?"
        cmp -s "$tmp" "$base.ansi" || { fail "$input (styled)"; diff "$base.ansi" "$tmp" | cat -v | head -20; }
    fi
done

expect_status() {
    want=$1
    shift
    "$@" > /dev/null 2>&1 < /dev/null
    got=$?
    [ "$got" = "$want" ] || fail "$* exited $got, expected $want"
}
expect_status 0 "$mark" --version
expect_status 0 "$mark" --help
expect_status 1 "$mark" "$dir/does-not-exist.md"
expect_status 1 "$mark" "$dir"
expect_status 2 "$mark" --width 0 "$dir/blocks.md"
expect_status 2 "$mark" --width 12x "$dir/blocks.md"
expect_status 2 "$mark" --bogus
expect_status 2 "$mark" "$dir/blocks.md" "$dir/inlines.md"
printf '# stdin\n' | "$mark" --plain | grep -qx stdin || fail "reading standard input"
printf '# stdin\n' | "$mark" --plain - | grep -qx stdin || fail "reading - "
printf 'a\r\nb\rc\0d' | "$mark" --plain | grep -q 'a b c�d' || fail "line endings and NUL"
printf '\357\273\277# bom\n' | "$mark" --plain | grep -qx bom || fail "byte order mark"
printf 'bad \377\376 bytes\n' | "$mark" --plain | grep -q 'bad �� bytes' || fail "invalid UTF-8"
printf '*em*\n' | NO_COLOR=1 "$mark" | grep -q "$(printf '\033\\[0;3mem')" || fail "NO_COLOR keeps attributes"
printf '`code`\n' | NO_COLOR=1 "$mark" | grep -q '40m' && fail "NO_COLOR drops colors"

# The config: written on first run, read after, typos reported by line.
[ -f "$XDG_CONFIG_HOME/mark/mark.conf" ] || fail "the sample config is written on first run"
cmp -s "$XDG_CONFIG_HOME/mark/mark.conf" "$dir/../mark.conf" || fail "the written config is the sample"
printf 'x\n' | "$mark" 2>&1 > /dev/null | grep -q . && fail "the sample config reads without complaints"
conf="$XDG_CONFIG_HOME/mark/mark.conf"
cp "$conf" "$tmp.conf"
printf '[layout]\nwidth = 20\n' > "$conf"
printf 'one two three four five six seven\n' | "$mark" --plain | head -1 | grep -qx 'one two three four' || fail "[layout] width"
printf 'one two three four five six seven\n' | "$mark" --plain --width 30 | head -1 | grep -qx 'one two three four five six' || fail "--width wins over the config"
printf '[layout]\nwidht = 20\n[colors]\nsource = rainbow\n' > "$conf"
printf 'x\n' | "$mark" 2>&1 > /dev/null | grep -q 'mark.conf:2: unknown key widht in \[layout\]' || fail "unknown keys are reported with the line"
printf 'x\n' | "$mark" 2>&1 > /dev/null | grep -q 'mark.conf:4: invalid value for source' || fail "invalid values are reported with the line"
expect_status 0 "$mark" "$dir/blocks.md"

# The palette: wall's roles in RGB, or ANSI colors with source = ansi or none.
mkdir -p "$XDG_CACHE_HOME/wall"
printf '[colors]\nbackground = #101010\nforeground = #f0f0f0\naccent = #aa0011\nhighlight = #00bb22\nmuted = #777777\nselection_background = #334455\nselection_foreground = #ffffff\nborder_active = #aa0011\nborder_inactive = #505050\nurgent = #ff0000\n' > "$XDG_CACHE_HOME/wall/colors.conf"
cp "$tmp.conf" "$conf"
printf '## heading\n\n`code`\n\n| a |\n|---|\n' | "$mark" > "$tmp"
grep -q '38;2;170;0;17mheading' "$tmp" || fail "the palette's accent colors headings"
grep -q '38;2;0;187;34;48;2;' "$tmp" || fail "the palette's highlight on a shade colors code"
grep -q '38;2;80;80;80m┌' "$tmp" || fail "the palette's border_inactive draws tables"
printf '[colors]\nsource = ansi\n' > "$conf"
printf '## heading\n' | "$mark" | grep -q '38;2' && fail "source = ansi uses no RGB"
printf '## heading\n' | NO_COLOR=1 "$mark" | grep -q '38;2' && fail "NO_COLOR drops the palette too"
cp "$tmp.conf" "$conf"
rm -f "$tmp.conf"

if command -v python3 > /dev/null; then
    python3 "$dir/commonmark.py" "$mark_test" || failed=1
else
    echo "python3 not found: skipping the CommonMark examples"
fi

[ "$failed" = 0 ] && echo "all tests passed"
exit "$failed"
