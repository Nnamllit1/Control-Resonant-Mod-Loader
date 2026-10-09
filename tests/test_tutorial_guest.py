"""Exercise the compiled tutorial example with real Wasmtime and input flags."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--bin', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temporary:
    mods = Path(temporary)/'mods'
    subprocess.run([sys.executable, str(root/'tools/mod.py'), 'build', str(root/'examples/tutorials'),
                    '--output', str(mods/'tutorials')], check=True)
    subprocess.run([str(args.bin.resolve()/'crml_tutorial_guest_tests.exe'), str(mods)], check=True)
