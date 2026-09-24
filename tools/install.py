"""Preview/install/remove an owned experimental loader without replacing game files."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import stat

ROOT = Path(__file__).resolve().parents[1]
FILES = ('xinput1_4.dll', 'crml/crml_runtime.dll', 'crml/wasmtime.dll',
         'crml/mods/hello/mod.ini', 'crml/mods/hello/hello.wasm',
         'crml/licenses/wasmtime.txt')
RECEIPT = 'crml/install-receipt.json'

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else hashlib.sha256(stream.read()).hexdigest()

def checked_path(root, relative):
    path = root / relative
    if not path.resolve().is_relative_to(root):
        raise ValueError(f'Path escapes installation: {relative}')
    for part in [path, *path.parents]:
        if part == root:
            break
        if part.exists() and getattr(part.lstat(), 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError(f'Linked installation path: {relative}')
    return path

def install(game, dist, profiles, apply=False):
    game = game.resolve(strict=True)
    executable = game / 'CONTROLResonant.exe'
    actual = digest(executable)
    if not any(p['sha256'] == actual and p['executable'] == executable.name for p in profiles):
        raise ValueError('Unknown game fingerprint; inspect and validate this build first')
    for relative in ('xinput1_4.dll', 'crml'):
        path = checked_path(game, relative)
        if path.exists():
            raise ValueError(f'Refusing existing {relative}; no files overwritten')
    sources = {name: dist / name for name in FILES}
    sources['crml/licenses/wasmtime.txt'] = dist / 'licenses/wasmtime/LICENSE'
    hashes = {name: digest(path) for name, path in sources.items()}
    print('Experimental profile: static inspection only; gameplay bridge unavailable')
    for name in FILES:
        print('ADD ' + str(checked_path(game, name)))
    if not apply:
        print('Preview only. Pass --apply with the game closed to install.')
        return
    created = []
    try:
        # Publish the proxy last, after the runtime and receipt are complete.
        for name in [n for n in FILES if n != 'xinput1_4.dll']:
            target = checked_path(game, name)
            target.parent.mkdir(parents=True, exist_ok=True)
            with sources[name].open('rb') as source, target.open('xb') as output:
                created.append(target)
                shutil.copyfileobj(source, output)
            if digest(target) != hashes[name]:
                raise ValueError(f'Copy verification failed: {name}')
        receipt = checked_path(game, RECEIPT)
        with receipt.open('x', encoding='utf-8') as output:
            created.append(receipt)
            json.dump({'schema': 1, 'executable_sha256': actual, 'files': hashes}, output, indent=2)
        target = checked_path(game, 'xinput1_4.dll')
        with sources['xinput1_4.dll'].open('rb') as source, target.open('xb') as output:
            created.append(target)
            shutil.copyfileobj(source, output)
        if digest(target) != hashes['xinput1_4.dll']:
            raise ValueError('Proxy copy verification failed')
    except Exception:
        # Only remove paths created by this invocation, never a pre-existing tree.
        for path in reversed(created):
            path.unlink(missing_ok=True)
        raise
    print('Installed. Start the game normally and inspect crml/crml.log.')

def uninstall(game, apply=False):
    game = game.resolve(strict=True)
    receipt_path = checked_path(game, RECEIPT)
    receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
    files = receipt.get('files', {})
    if receipt.get('schema') != 1 or set(files) != set(FILES):
        raise ValueError('Invalid installation receipt')
    for name, expected in files.items():
        path = checked_path(game, name)
        if not path.is_file() or digest(path) != expected:
            raise ValueError(f'Owned file missing or modified; preserved: {name}')
        print('REMOVE ' + str(path))
    if not apply:
        print('Preview only. Pass --apply with the game closed to remove owned files.')
        return
    # Disable the loading route first; leave all other files if it is in use.
    checked_path(game, 'xinput1_4.dll').unlink()
    for name in FILES:
        if name != 'xinput1_4.dll':
            checked_path(game, name).unlink()
    receipt_path.unlink()
    print('Removed owned files. Additional mods, logs, and directories were preserved.')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--dist', type=Path, default=ROOT if (ROOT / 'xinput1_4.dll').exists() else ROOT / 'dist')
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--uninstall', action='store_true')
    args = parser.parse_args()
    try:
        if args.uninstall:
            uninstall(args.game, args.apply)
        else:
            profiles = json.loads((ROOT / 'compatibility.json').read_text())['profiles']
            install(args.game, args.dist, profiles, args.apply)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f'{error}\n')

if __name__ == '__main__':
    main()
