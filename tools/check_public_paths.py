"""Reject machine-local filesystem paths in repository text or generated documentation.

With no arguments, inspect tracked and non-ignored untracked repository files.
Diagnostics name only the relative file and line; matched private text is never printed.
This is a path check, not a general secret or personal-information scanner.
"""
import argparse
from html import unescape
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PATTERNS = (
    re.compile(r'(?<![A-Za-z0-9])[A-Za-z]:(?:\\+|/(?!/))'),
    re.compile(r'(?<![\\\w.])\\\\[A-Za-z0-9_.-]+\\[A-Za-z0-9_$.-]+'),
    re.compile(r'(?<![\w:])/(?:home|Users|mnt|media|Volumes)/[^\s<>"\']+'),
)


def strings(value):
    if isinstance(value, str): yield value
    elif isinstance(value, list):
        for item in value: yield from strings(item)
    elif isinstance(value, dict):
        for key, item in value.items():
            yield key
            yield from strings(item)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='*', type=Path)
    args = parser.parse_args()
    if args.paths:
        paths = {p for item in args.paths for p in (item.rglob('*') if item.is_dir() else [item]) if p.is_file()}
        if not paths:
            parser.error('No files found to inspect')
    else:
        names = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=ROOT)
        paths = {ROOT / name.decode('utf-8') for name in names.split(b'\0') if name}
    failed = 0
    checked = 0
    for path in sorted(paths):
        if not path.is_file(): continue
        # MkDocs' vendor bundles contain regexes and generic OS path examples.
        # Their source maps are not project documentation or machine-local build output.
        if path.suffix in ('.js', '.map') and path.resolve().is_relative_to(ROOT / 'site' / 'assets'):
            continue
        try:
            text = path.read_text(encoding='utf-8')
        except UnicodeDecodeError:
            continue
        checked += 1
        # Decode JSON strings so escaped repository-relative paths are not mistaken for UNC shares.
        if path.suffix == '.json':
            values = [(None, value) for value in strings(json.loads(text))]
        else:
            values = enumerate(text.splitlines(), 1)
        for line, value in values:
            if any(pattern.search(unescape(value)) for pattern in PATTERNS):
                try:
                    label = path.resolve().relative_to(ROOT).as_posix()
                except ValueError:
                    label = '<external file>'
                location = f'{label}:{line}' if line else label
                print(f'{location}: local filesystem path; use a variable or relative path')
                failed += 1
    print(f'Checked {checked} text files; {failed} lines require path removal.')
    return bool(failed)


if __name__ == '__main__':
    raise SystemExit(main())
