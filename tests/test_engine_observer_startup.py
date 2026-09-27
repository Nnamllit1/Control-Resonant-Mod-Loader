"""Load the real runtime in a non-game process to exercise fail-closed startup."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def child(runtime):
    import ctypes
    import os
    with os.add_dll_directory(str(runtime.parent)):
        module = ctypes.WinDLL(str(runtime))
        module.crml_run.restype = ctypes.c_uint32
        return module.crml_run()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--child', action='store_true')
    args = parser.parse_args()
    if args.child:
        return child(args.runtime.resolve())
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        runtime = root / 'crml_runtime.dll'
        shutil.copyfile(args.runtime, runtime)
        shutil.copyfile(args.runtime.parent / 'wasmtime.dll', root / 'wasmtime.dll')
        (root / 'engine-observer.enabled').write_text('')
        # Even conflicting existing opt-ins must not start those features.
        for name in ('noclip.enabled', 'visibility.enabled', 'entity-inspector.enabled'):
            (root / name).write_text('')
        # A normal Runtime::load would diagnose this malformed manifest.
        mod = root / 'mods' / 'must-not-load'
        mod.mkdir(parents=True)
        (mod / 'mod.ini').write_text('[mod]\ninvalid-key=this-package-must-not-be-scanned\n')
        subprocess.run([sys.executable, str(Path(__file__).resolve()), '--child', '--runtime', str(runtime)],
                       check=True, timeout=20, capture_output=True)
        log = (root / 'crml.log').read_text()
        assert 'Wasm mods suspended for engine observation' in log, log
        assert 'Engine observer refused: unsupported executable fingerprint' in log, log
        assert 'must-not-load' not in log and 'Movement probe' not in log, log
        assert not list(root.glob('engine-observer-*.jsonl'))
        assert not (root / 'movement-probe.jsonl').exists()
        (root / 'engine-observer.enabled').unlink()
        (root / 'physics-trial.enabled').write_text('')
        subprocess.run([sys.executable, str(Path(__file__).resolve()), '--child', '--runtime', str(runtime)],
                       check=True, timeout=20, capture_output=True)
        log = (root / 'crml.log').read_text()
        assert 'Wasm mods suspended for native physics trial' in log, log
        assert 'Physics trial refused: unsupported executable fingerprint' in log, log
        assert 'must-not-load' not in log and not list(root.glob('physics-trial-*.jsonl'))
        (root / 'engine-observer.enabled').write_text('')
        subprocess.run([sys.executable, str(Path(__file__).resolve()), '--child', '--runtime', str(runtime)],
                       check=True, timeout=20, capture_output=True)
        log = (root / 'crml.log').read_text()
        assert 'conflicting observer and physics trial markers' in log and 'must-not-load' not in log, log
        (root / 'engine-observer.enabled').unlink()
        (root / 'physics-trial.enabled').unlink()
        (root / 'physics-wasm.enabled').write_text('')
        subprocess.run([sys.executable, str(Path(__file__).resolve()), '--child', '--runtime', str(runtime)],
                       check=True, timeout=20, capture_output=True)
        log = (root / 'crml.log').read_text()
        assert 'Physics trial refused: unsupported executable fingerprint' in log, log
        assert 'must-not-load' not in log and 'Movement probe' not in log, log
        (root / 'physics-trial.enabled').write_text('')
        subprocess.run([sys.executable, str(Path(__file__).resolve()), '--child', '--runtime', str(runtime)],
                       check=True, timeout=20, capture_output=True)
        log = (root / 'crml.log').read_text()
        assert 'conflicting observer and physics trial markers' in log and 'must-not-load' not in log, log
        print('Real-runtime unsupported-host refusal and observe-only startup isolation passed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
