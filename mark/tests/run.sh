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

if command -v python3 > /dev/null; then
    python3 "$dir/commonmark.py" "$mark_test" || failed=1
else
    echo "python3 not found: skipping the CommonMark examples"
fi

[ "$failed" = 0 ] && echo "all tests passed"
exit "$failed"
