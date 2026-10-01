"""Preview, install, update, or remove owned loader files without replacing game files."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import uuid

ROOT = Path(__file__).resolve().parents[1]
FILES = ('xinput1_4.dll', 'crml/crml_runtime.dll', 'crml/wasmtime.dll',
         'crml/mods/hello/mod.ini', 'crml/mods/hello/hello.wasm',
         'crml/licenses/wasmtime.txt')
RECEIPT = 'crml/install-receipt.json'
INSPECTOR_FILES = {'crml/entity-inspector.enabled': 'crml/entity-inspector.enabled'}
OBSERVER_FILES = {'crml/engine-observer.enabled': 'crml/engine-observer.enabled'}
PHYSICS_FILES = {'crml/physics-trial.enabled': 'crml/physics-trial.enabled'}
PHYSICS_WASM_FILES = {'crml/physics-wasm.enabled': 'crml/physics-wasm.enabled'}
MOVEMENT_WASM_FILES = {'crml/movement-wasm.enabled': 'crml/movement-wasm.enabled'}
LUA_SOURCE_FILES = {'crml/engine-lua.enabled': 'crml/engine-lua.enabled'}
OPTIONAL_FILES = {'crml/licenses/minhook.txt': 'licenses/minhook/LICENSE.txt',
                  'crml/licenses/luau.txt': 'licenses/luau/LICENSE.txt',
                  'crml/ui-bootstrap.html': 'crml/ui-bootstrap.html'}
NOCLIP_FILES = {'crml/noclip.enabled': 'examples/noclip/noclip.enabled',
                'crml/mods/noclip/mod.ini': 'examples/noclip/mod.ini',
                'crml/mods/noclip/noclip.wasm': 'examples/noclip/noclip.wasm'}

VISIBILITY_FILES = {'crml/visibility.enabled': 'examples/visibility/visibility.enabled',
                    'crml/mods/visibility/mod.ini': 'examples/visibility/mod.ini',
                    'crml/mods/visibility/visibility.wasm': 'examples/visibility/visibility.wasm'}


def sources_for(dist, experimental_noclip=False, entity_inspector=False, experimental_visibility=False, engine_observer=False, physics_trial=False, physics_wasm=False, movement_wasm=False, lua_source=False):
    sources = {name: dist / name for name in FILES}
    sources['crml/licenses/wasmtime.txt'] = dist / 'licenses/wasmtime/LICENSE'
    sources.update({name: dist / source for name, source in OPTIONAL_FILES.items() if (dist / source).is_file()})
    if lua_source:
        if not movement_wasm:
            raise ValueError('Trusted Lua sources require --movement-wasm in this development build')
        features = json.loads((dist / 'crml/build-features.json').read_text(encoding='utf-8-sig'))
        if features.get('lua_source') is not True:
            raise ValueError('Rebuild with -LuaSource before enabling trusted Lua source mods')
        sources.update({name: dist / source for name, source in LUA_SOURCE_FILES.items()})
        sources['crml/licenses/luau.txt'] = dist / 'licenses/luau/LICENSE.txt'
    if movement_wasm:
        if physics_wasm or physics_trial or engine_observer or experimental_noclip or entity_inspector or experimental_visibility:
            raise ValueError('Wasm movement must be installed without other experimental modes')
        features = json.loads((dist / 'crml/build-features.json').read_text(encoding='utf-8-sig'))
        if features.get('movement_wasm') is not True:
            raise ValueError('Rebuild with -ExperimentalGameplay before installing Wasm movement')
        sources.update({name: dist / source for name, source in MOVEMENT_WASM_FILES.items()})
    if physics_wasm:
        if physics_trial or engine_observer or experimental_noclip or entity_inspector or experimental_visibility:
            raise ValueError('Wasm physics must be installed without other experimental modes')
        features = json.loads((dist / 'crml/build-features.json').read_text(encoding='utf-8-sig'))
        if features.get('physics_wasm') is not True:
            raise ValueError('Rebuild with -EngineObserver before installing Wasm physics')
        sources.update({name: dist / source for name, source in PHYSICS_WASM_FILES.items()})
    if physics_trial:
        if engine_observer or experimental_noclip or entity_inspector or experimental_visibility:
            raise ValueError('Physics trial must be installed without other experimental modes')
        features = json.loads((dist / 'crml/build-features.json').read_text(encoding='utf-8-sig'))
        if features.get('physics_trial') is not True:
            raise ValueError('Rebuild with -EngineObserver before installing the physics trial')
        sources.update({name: dist / source for name, source in PHYSICS_FILES.items()})
    if engine_observer:
        if experimental_noclip or entity_inspector or experimental_visibility:
            raise ValueError('Engine observation must be installed without other experimental modes')
        features = json.loads((dist / 'crml/build-features.json').read_text(encoding='utf-8-sig'))
        if features.get('engine_observer') is not True:
            raise ValueError('Rebuild with -EngineObserver before installing engine observation')
        sources.update({name: dist / source for name, source in OBSERVER_FILES.items()})
    if experimental_noclip or entity_inspector or experimental_visibility:
        features = json.loads((dist / 'crml/build-features.json').read_text(encoding='utf-8-sig'))
        if features.get('experimental_gameplay') is not True:
            raise ValueError('Rebuild with -ExperimentalGameplay before installing experimental gameplay features')
        if experimental_noclip:
            sources.update({name: dist / source for name, source in NOCLIP_FILES.items()})
        if experimental_visibility:
            sources.update({name: dist / source for name, source in VISIBILITY_FILES.items()})
        if entity_inspector:
            sources.update({name: dist / source for name, source in INSPECTOR_FILES.items()})
    return sources


def require_closed(game):
    """Refuse updates while this exact installation is running."""
    if os.name != 'nt':
        return
    import ctypes as c
    from ctypes import wintypes as w
    class Entry(c.Structure):
        _fields_ = [('size', w.DWORD), ('usage', w.DWORD), ('pid', w.DWORD), ('heap', c.c_size_t),
                    ('module', w.DWORD), ('threads', w.DWORD), ('parent', w.DWORD), ('priority', w.LONG),
                    ('flags', w.DWORD), ('exe', w.WCHAR * 260)]
    k = c.WinDLL('kernel32', use_last_error=True)
    k.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
    k.CreateToolhelp32Snapshot.restype = w.HANDLE
    k.Process32FirstW.argtypes = k.Process32NextW.argtypes = [w.HANDLE, c.POINTER(Entry)]
    k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    k.OpenProcess.restype = w.HANDLE
    k.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
    k.CloseHandle.argtypes = [w.HANDLE]
    snapshot = k.CreateToolhelp32Snapshot(2, 0)
    if snapshot == c.c_void_p(-1).value:
        raise OSError('Cannot check whether the game is running')
    entry = Entry()
    entry.size = c.sizeof(entry)
    try:
        more = k.Process32FirstW(snapshot, c.byref(entry))
        while more:
            if entry.exe.lower() == 'controlresonant.exe':
                process = k.OpenProcess(0x1000, False, entry.pid)
                if not process:
                    raise ValueError('Close CONTROL Resonant before changing the installation')
                try:
                    path = c.create_unicode_buffer(32768)
                    length = w.DWORD(len(path))
                    if not k.QueryFullProcessImageNameW(process, 0, path, c.byref(length)) or Path(path.value).resolve().parent == game:
                        raise ValueError('Close CONTROL Resonant before changing the installation')
                finally:
                    k.CloseHandle(process)
            more = k.Process32NextW(snapshot, c.byref(entry))
        if c.get_last_error() != 18:  # ERROR_NO_MORE_FILES
            raise OSError('Could not finish checking whether the game is running')
    finally:
        k.CloseHandle(snapshot)

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else hashlib.sha256(stream.read()).hexdigest()

def require_x64_executable(path):
    """Permit untested PE32+ AMD64 builds; execution still requires in-game consent."""
    with path.open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b'MZ':
            raise ValueError('Unknown game file is not a Windows x64 executable')
        offset = struct.unpack_from('<I', header, 60)[0]
        if offset < 64 or offset > path.stat().st_size - 26:
            raise ValueError('Unknown game executable has invalid PE headers')
        stream.seek(offset)
        pe = stream.read(26)
        if pe[:4] != b'PE\0\0' or struct.unpack_from('<H', pe, 4)[0] != 0x8664 or struct.unpack_from('<H', pe, 24)[0] != 0x20b:
            raise ValueError('Unknown game file is not a Windows x64 executable')

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

def install(game, dist, profiles, apply=False, experimental_noclip=False, entity_inspector=False, experimental_visibility=False, engine_observer=False, physics_trial=False, physics_wasm=False, movement_wasm=False, lua_source=False):
    game = game.resolve(strict=True)
    executable = game / 'CONTROLResonant.exe'
    actual = digest(executable)
    profile = next((p for p in profiles if p['sha256'] == actual and p['executable'] == executable.name), None)
    if profile is None:
        require_x64_executable(executable)
        profile = {'status': 'untested executable; CRML will ask before starting mods'}
    for relative in ('xinput1_4.dll', 'crml'):
        path = checked_path(game, relative)
        if path.exists():
            raise ValueError(f'Refusing existing {relative}; no files overwritten')
    sources = sources_for(dist, experimental_noclip, entity_inspector, experimental_visibility, engine_observer, physics_trial, physics_wasm, movement_wasm, lua_source)
    hashes = {name: digest(path) for name, path in sources.items()}
    print('Experimental profile: ' + profile.get('status', 'unverified'))
    if experimental_noclip:
        print('Experimental noclip selected; live gameplay remains unverified')
    for name in sources:
        print('ADD ' + str(checked_path(game, name)))
    if not apply:
        print('Preview only. Pass --apply with the game closed to install.')
        return
    created = []
    require_closed(game)
    try:
        # Publish the proxy last, after the runtime and receipt are complete.
        for name in [n for n in sources if n != 'xinput1_4.dll']:
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

def read_receipt(game):
    receipt_path = checked_path(game, RECEIPT)
    receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
    files = receipt.get('files', {})
    if receipt.get('schema') != 1 or not set(FILES).issubset(files) or set(files) - set(FILES) - set(OPTIONAL_FILES) - set(NOCLIP_FILES) - set(INSPECTOR_FILES) - set(VISIBILITY_FILES) - set(OBSERVER_FILES) - set(PHYSICS_FILES) - set(PHYSICS_WASM_FILES) - set(MOVEMENT_WASM_FILES) - set(LUA_SOURCE_FILES):
        raise ValueError('Invalid installation receipt')
    for name, expected in files.items():
        path = checked_path(game, name)
        if not path.is_file() or digest(path) != expected:
            raise ValueError(f'Owned file missing or modified; preserved: {name}')
    return receipt


def uninstall(game, apply=False):
    game = game.resolve(strict=True)
    files = read_receipt(game)['files']
    for name in files:
        print('REMOVE ' + str(checked_path(game, name)))
    if not apply:
        print('Preview only. Pass --apply with the game closed to remove owned files.')
        return
    require_closed(game)
    # Disable the loading route first; leave all other files if it is in use.
    checked_path(game, 'xinput1_4.dll').unlink()
    for name in files:
        if name != 'xinput1_4.dll':
            checked_path(game, name).unlink()
    checked_path(game, RECEIPT).unlink()
    print('Removed owned files. Additional mods, logs, and directories were preserved.')

def update(game, dist, profiles, apply=False, experimental_noclip=False, entity_inspector=False, experimental_visibility=False, engine_observer=False, disable_engine_observer=False, physics_trial=False, disable_physics_trial=False, physics_wasm=False, disable_physics_wasm=False, movement_wasm=False, disable_movement_wasm=False, lua_source=False, disable_lua_source=False):
    game = game.resolve(strict=True)
    receipt = read_receipt(game)
    if engine_observer and disable_engine_observer:
        raise ValueError('Cannot enable and disable engine observation together')
    actual = digest(game / 'CONTROLResonant.exe')
    if not any(p['sha256'] == actual and p['executable'] == 'CONTROLResonant.exe' for p in profiles):
        require_x64_executable(game / 'CONTROLResonant.exe')
        print('Untested executable; CRML will ask before starting mods.')
    if physics_trial and disable_physics_trial:
        raise ValueError('Cannot enable and disable physics trial together')
    if physics_wasm and disable_physics_wasm:
        raise ValueError('Cannot enable and disable Wasm physics together')
    if lua_source and disable_lua_source:
        raise ValueError('Cannot enable and disable Lua sources together')
    if movement_wasm and disable_movement_wasm:
        raise ValueError('Cannot enable and disable Wasm movement together')
    sources = sources_for(dist, experimental_noclip, entity_inspector, experimental_visibility, engine_observer, physics_trial, physics_wasm, movement_wasm, lua_source)
    hashes = {name: digest(source) for name, source in sources.items()}
    changes = {name: source for name, source in sources.items() if receipt['files'].get(name) != hashes[name]}
    removals = set(OBSERVER_FILES) & receipt['files'].keys() if disable_engine_observer else set()
    switched_off = (set(OBSERVER_FILES) | set(PHYSICS_FILES)) if physics_wasm else set()
    if movement_wasm:
        switched_off |= set(OBSERVER_FILES) | set(PHYSICS_FILES) | set(PHYSICS_WASM_FILES)
    if physics_wasm or physics_trial or engine_observer or disable_movement_wasm or experimental_noclip or experimental_visibility or entity_inspector:
        switched_off |= set(MOVEMENT_WASM_FILES) | set(LUA_SOURCE_FILES)
    if physics_trial or engine_observer or disable_physics_wasm:
        switched_off |= set(PHYSICS_WASM_FILES)
    for name in switched_off:
        if checked_path(game, name).exists() and name not in receipt['files']:
            raise ValueError(f'Remove the unowned mode marker before switching: {name}')
    if disable_lua_source:
        switched_off |= set(LUA_SOURCE_FILES)
        for name in LUA_SOURCE_FILES:
            if checked_path(game, name).exists() and name not in receipt['files']:
                raise ValueError('Refusing unowned Lua source opt-in marker')
    removals |= switched_off & receipt['files'].keys()
    if physics_trial:
        removals |= set(OBSERVER_FILES) & receipt['files'].keys()
        unowned = checked_path(game, 'crml/engine-observer.enabled')
        if unowned.exists() and 'crml/engine-observer.enabled' not in receipt['files']:
            raise ValueError('Remove the unowned engine observer marker before enabling physics trial')
    if engine_observer or disable_physics_trial:
        removals |= set(PHYSICS_FILES) & receipt['files'].keys()
    if engine_observer and checked_path(game, 'crml/physics-trial.enabled').exists() and 'crml/physics-trial.enabled' not in receipt['files']:
        raise ValueError('Remove the unowned physics trial marker before enabling engine observer')
    for name in removals:
        print('REMOVE ' + str(checked_path(game, name)))
    for name in changes:
        target = checked_path(game, name)
        if name not in receipt['files'] and target.exists():
            raise ValueError(f'Refusing existing unowned file: {name}')
        print('UPDATE ' + str(target))
    if not apply:
        print('Preview only. Pass --update --apply with the game closed to update.')
        return
    require_closed(game)
    if not changes and not removals and actual == receipt.get('executable_sha256'):
        print('Already up to date.')
        return
    token = uuid.uuid4().hex
    stages, backups, published = {}, {}, []
    succeeded = False
    try:
        for name, source in changes.items():
            target = checked_path(game, name)
            target.parent.mkdir(parents=True, exist_ok=True)
            stage = target.with_name(target.name + '.' + token + '.new')
            stages[name] = stage
            with source.open('rb') as inp, stage.open('xb') as out:
                shutil.copyfileobj(inp, out)
            if digest(stage) != hashes[name]:
                raise ValueError(f'Copy verification failed: {name}')
        for name in removals:
            stages[name] = None
        new_receipt = dict(receipt, executable_sha256=actual, files={name: value for name, value in {**receipt['files'], **hashes}.items() if name not in removals})
        target = checked_path(game, RECEIPT)
        stages[RECEIPT] = target.with_name(target.name + '.' + token + '.new')
        with stages[RECEIPT].open('x', encoding='utf-8') as out:
            json.dump(new_receipt, out, indent=2)
        for name, stage in stages.items():
            target = checked_path(game, name)
            if target.exists():
                backup = target.with_name(target.name + '.' + token + '.backup')
                target.rename(backup)
                backups[name] = backup
            if stage is not None:
                stage.replace(target)
            published.append(name)
        succeeded = True
    except Exception:
        # Restore the receipt and every replaced file. Keep backups if restoration fails.
        for name in reversed(list(stages)):
            target = checked_path(game, name)
            if name in backups:
                backups[name].replace(target)
            elif name in published:
                target.unlink()
        raise
    finally:
        for stage in stages.values():
            if stage is not None:
                stage.unlink(missing_ok=True)
        if succeeded:
            for backup in backups.values():
                backup.unlink()
    print('Updated owned files; additional mods, settings, and logs were preserved.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--dist', type=Path, default=ROOT if (ROOT / 'xinput1_4.dll').exists() else ROOT / 'dist')
    parser.add_argument('--apply', action='store_true')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--uninstall', action='store_true')
    mode.add_argument('--update', action='store_true')
    parser.add_argument('--experimental-noclip', action='store_true', help='Install the opt-in experimental noclip example and enable its native bridge')
    parser.add_argument('--entity-inspector', action='store_true', help='Enable read-only player inspection; takes precedence over noclip at runtime')
    parser.add_argument('--experimental-visibility', action='store_true', help='Install the Wasm visibility example and enable hold-F7 player mesh hiding')
    source_mode = parser.add_mutually_exclusive_group()
    source_mode.add_argument('--lua-source', action='store_true', help='With --movement-wasm, opt into trusted engine Lua source mods; these are not sandboxed')
    source_mode.add_argument('--disable-lua-source', action='store_true', help='With --update, remove the owned Lua source opt-in marker; preserves source packages')
    observer_mode = parser.add_mutually_exclusive_group()
    observer_mode.add_argument('--engine-observer', action='store_true', help='Enable observe-only engine validation; suspends all mods and other gameplay modes')
    observer_mode.add_argument('--disable-engine-observer', action='store_true', help='With --update, remove the owned observer marker and restore ordinary startup')
    observer_mode.add_argument('--physics-trial', action='store_true', help='Enable the native reversible damping trial; suspends Wasm mods')
    observer_mode.add_argument('--disable-physics-trial', action='store_true', help='With --update, remove the owned physics trial marker')
    observer_mode.add_argument('--physics-wasm', action='store_true', help='Enable the isolated bounded Wasm physics service')
    observer_mode.add_argument('--disable-physics-wasm', action='store_true', help='With --update, remove the owned Wasm physics marker')
    observer_mode.add_argument('--movement-wasm', action='store_true', help='Enable guest-controlled movement without legacy fall/reset overrides')
    observer_mode.add_argument('--disable-movement-wasm', action='store_true', help='With --update, remove the owned Wasm movement marker')
    args = parser.parse_args()
    if args.disable_lua_source and not args.update:
        parser.error('--disable-lua-source requires --update')
    if args.disable_engine_observer and not args.update:
        parser.error('--disable-engine-observer requires --update')
    if args.disable_physics_trial and not args.update:
        parser.error('--disable-physics-trial requires --update')
    if args.disable_physics_wasm and not args.update:
        parser.error('--disable-physics-wasm requires --update')
    if args.disable_movement_wasm and not args.update:
        parser.error('--disable-movement-wasm requires --update')
    try:
        if args.uninstall:
            uninstall(args.game, args.apply)
        else:
            profiles = json.loads((ROOT / 'compatibility.json').read_text())['profiles']
            action = update if args.update else install
            options = {'disable_engine_observer': args.disable_engine_observer, 'disable_physics_trial': args.disable_physics_trial, 'disable_physics_wasm': args.disable_physics_wasm, 'disable_movement_wasm': args.disable_movement_wasm} if args.update else {}
            options['physics_trial'] = args.physics_trial
            options['physics_wasm'] = args.physics_wasm
            options['movement_wasm'] = args.movement_wasm
            options['lua_source'] = args.lua_source
            if args.update:
                options['disable_lua_source'] = args.disable_lua_source
            action(args.game, args.dist, profiles, args.apply, args.experimental_noclip, args.entity_inspector, args.experimental_visibility, args.engine_observer, **options)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f'{error}\n')

if __name__ == '__main__':
    main()
