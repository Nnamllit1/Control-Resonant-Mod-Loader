"""Test named/ordinal exports, forwarding, and deferred runtime startup."""
import argparse
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

def probe(path):
    proxy = ctypes.WinDLL(str(path))
    system = ctypes.WinDLL(str(Path(os.environ['SystemRoot']) / 'System32' / 'xinput1_4.dll'), winmode=0x800)
    for name in ['XInputGetState', 'XInputSetState', 'XInputGetCapabilities', 'XInputEnable', 'XInputGetBatteryInformation', 'XInputGetKeystroke', 'XInputGetAudioDeviceIds']:
        getattr(proxy, name)
    for ordinal in [2, 3, 4, 5, 7, 8, 10, 100, 101, 102, 103, 104, 108, 109]:
        proxy[ordinal]
    for library in [proxy, system]:
        for fn in [library[2], library.XInputGetState]:
            fn.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
            fn.restype = ctypes.c_uint32
    state = ctypes.create_string_buffer(16)
    expected = system.XInputGetState(999, state)
    if proxy[2](999, state) != expected or proxy.XInputGetState(999, state) != expected:
        raise RuntimeError('Ordinal/name forwarding mismatch')
    for library in [proxy, system]:
        library.XInputGetCapabilities.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        library.XInputGetCapabilities.restype = ctypes.c_uint32
        library.XInputGetAudioDeviceIds.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        library.XInputGetAudioDeviceIds.restype = ctypes.c_uint32
    buffer = ctypes.create_string_buffer(512)
    if proxy.XInputGetCapabilities(999, 0, buffer) != system.XInputGetCapabilities(999, 0, buffer):
        raise RuntimeError('Capabilities forwarding mismatch')
    a, b = ctypes.c_uint32(), ctypes.c_uint32()
    args = (999, None, ctypes.byref(a), None, ctypes.byref(b))
    if proxy.XInputGetAudioDeviceIds(*args) != system.XInputGetAudioDeviceIds(*args):
        raise RuntimeError('Five-argument forwarding mismatch')
    log = path.parent / 'crml' / 'crml.log'
    for _ in range(100):
        if log.exists() and 'Hello from a sandboxed' in log.read_text():
            print('Proxy forwarding and deferred Wasm startup passed')
            return
        time.sleep(0.05)
    raise RuntimeError('Deferred runtime did not load the example mod')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin', type=Path)
    parser.add_argument('--probe', type=Path)
    args = parser.parse_args()
    if args.probe:
        probe(args.probe)
        return
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(dir=args.bin.parent) as directory:
        stage = Path(directory)
        runtime = stage / 'crml'
        hello = runtime / 'mods' / 'hello'
        hello.mkdir(parents=True)
        shutil.copy2(args.bin / 'xinput1_4.dll', stage)
        for name in ['crml_runtime.dll', 'wasmtime.dll']:
            shutil.copy2(args.bin / name, runtime)
        shutil.copy2(root / 'examples' / 'hello' / 'mod.ini', hello)
        subprocess.run([str(args.bin / 'crml_wat.exe'), str(root / 'examples' / 'hello' / 'hello.wat'), str(hello / 'hello.wasm')], check=True)
        subprocess.run([sys.executable, __file__, '--probe', str(stage / 'xinput1_4.dll')], check=True, timeout=15)

if __name__ == '__main__':
    main()
