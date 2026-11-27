#!/usr/bin/env python3
"""Compare mark's parser with the CommonMark spec examples and time the
inputs that make naive parsers quadratic. Run by `make check`.

usage: commonmark.py MARK_TEST_BINARY [SECTION]"""
import json
import os
import subprocess
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
binary = sys.argv[1]
show = sys.argv[2] if len(sys.argv) > 2 else None


def run(args, text, timeout=10):
    return subprocess.run([binary] + args + ['-'], input=text.encode(),
                          capture_output=True, timeout=timeout)


failures = 0
tests = json.load(open(os.path.join(here, 'spec.json'), encoding='utf-8'))
passed = 0
for t in tests:
    out = run(['--html', '--commonmark'], t['markdown']).stdout.decode()
    if out == t['html']:
        passed += 1
    elif show in (t['section'], 'all'):
        print(f"example {t['example']} ({t['section']})")
        print('  input:', repr(t['markdown']))
        print('  want: ', repr(t['html']))
        print('  got:  ', repr(out))
print(f"CommonMark {tests[0].get('version', '0.31.2')}: {passed}/{len(tests)} examples")
if passed != len(tests):
    failures += 1

# GitHub's examples for the extensions mark supports, from the GFM spec
# (cmark-gfm's test/spec.txt).
gfm = json.load(open(os.path.join(here, 'gfm.json'), encoding='utf-8'))
passed = 0
for t in gfm:
    out = run(['--html'], t['markdown']).stdout.decode()
    if out == t['html']:
        passed += 1
    elif show in (t['extension'], 'all'):
        print(f"{t['extension']} example")
        print('  input:', repr(t['markdown']))
        print('  want: ', repr(t['html']))
        print('  got:  ', repr(out))
print(f"GitHub extensions: {passed}/{len(gfm)} examples")
if passed != len(gfm):
    failures += 1

pathological = {
    'nested strong emphasis': '*a **a ' * 65000 + 'b' + ' a** a*' * 65000,
    'emphasis closers without openers': 'a_ ' * 65000,
    'emphasis openers without closers': '_a ' * 65000,
    'link closers without openers': 'a]' * 65000,
    'link openers without closers': '[a' * 65000,
    'mismatched openers and closers': '*a_ ' * 50000,
    'openers and closers multiple of 3': 'a**b' + 'c* ' * 50000,
    'link openers and emphasis closers': '[ a_' * 50000,
    'pattern [ (](': '[ (](' * 80000,
    'nested brackets': '[' * 50000 + 'a' + ']' * 50000,
    'nested block quotes': '> ' * 50000 + 'a',
    'deeply nested lists': ''.join('  ' * i + '* a\n' for i in range(1000)),
    'backticks': ''.join('e' + '`' * i for i in range(1, 5000)),
    'unclosed links A': '[a](<b' * 30000,
    'unclosed links B': '[a](b' * 30000,
    'unclosed comments': '</' + '<!--' * 100000,
    'unclosed attribute values': '<a b="' * 50000,
    'unclosed processing instructions': 'a <?' * 50000,
    'many references': ''.join(f'[{i}]: u\n' for i in range(50000)) + '[0] ' * 50000,
    'nested images': '![' * 30000 + 'a' + '](b)' * 30000,
    'one long line': 'word ' * 400000,
    'wide characters': '漢字かな🙂 ' * 100000,
    'many short lines': 'a\n' * 300000,
    'many table rows': '| a | b |\n|---|---|\n' + '| c | d |\n' * 100000,
    'many table columns': '|a' * 5000 + '|\n' + '|-' * 5000 + '|\n' + '|b' * 5000 + '|\n',
    'wide table': '|' + ' x |' * 200 + '\n|' + '---|' * 200 + '\n' + ('|' + ' word word |' * 200 + '\n') * 200,
    'many tildes': '~a ~~b ' * 50000,
    'many bare links': 'www.example.com ' * 50000,
    'many at signs': 'a@b ' * 50000 + '@' * 100000,
    'adjacent text nodes': 'a]&' * 60000,
}
for name, text in pathological.items():
    for args in (['--html'], ['--width', '80']):
        start = time.monotonic()
        try:
            result = run(args, text)
            ok = result.returncode == 0
        except subprocess.TimeoutExpired:
            ok = False
        elapsed = time.monotonic() - start
        if not ok or elapsed > 2:
            print(f'pathological: {name} ({args[0]}): {"failed" if not ok else f"{elapsed:.1f} s"}')
            failures += 1
print('pathological inputs: done')
sys.exit(1 if failures else 0)
