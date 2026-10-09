"""Real Wasmtime boundary, ownership and lifecycle tests for passive drawing."""
import argparse
from contextlib import contextmanager
from pathlib import Path
import struct
import subprocess
import sys
import shutil
import uuid


@contextmanager
def scenario_directory(parent):
    # Python 3.14's Windows mkdtemp mode=0700 can deny the sandbox token access.
    # Inherit the writable workspace ACL and retain small Wasm failure artifacts.
    path=parent/('drawing-guest-'+uuid.uuid4().hex)
    path.mkdir()
    yield path


def descriptor(label='A'):
    data = bytearray(5680)
    struct.pack_into('<8I4f', data, 0, 1, 5680, 1000, 1, 1, 0, 0, 0, 0, 0, .5, .5)
    struct.pack_into('<5fI', data, 48, .1, .1, .9, .9, .2, 0xffffffff)
    struct.pack_into('<3fI', data, 3120, .1, .1, 2, 0xffffffff)
    data[3136:3136+len(label)] = label.encode('utf-8')
    return data


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin', type=Path, required=True)
    args = parser.parse_args()
    binary = args.bin.resolve()
    root = Path(__file__).resolve().parents[1]
    temp = root/'.local'/'test-tmp'
    temp.mkdir(parents=True, exist_ok=True)
    publish = 'i32.const 0 i32.const 5680 call $publish'
    good = publish+' i32.const 1 call $expect'
    cases = 0
    with scenario_directory(temp) as folder:
        work = Path(folder)

        def package(name, init=good, tick='', shutdown='', caps='drawing', memory=True, abi='i32.const 1', start='', data=None):
            mods = work/name
            mod = mods/name
            mod.mkdir(parents=True)
            body = data if data is not None else descriptor(name)
            escaped = ''.join(f'\\{b:02x}' for b in body)
            source = f'''(module
                (import "crml_v1" "drawing_publish" (func $publish (param i32 i32) (result i32)))
                (import "crml_v1" "drawing_hide" (func $hide (result i32)))
                (import "crml_v1" "map_projection_read" (func $projection (param i32 i32) (result i32)))
                (import "crml_v1" "map_read" (func $map_read (param i32 i32) (result i32)))
                (import "crml_v1" "map_publish" (func $map_publish (param i32 i32) (result i32)))
                (import "crml_v1" "map_hide" (func $map_hide (result i32)))
                (import "crml_v1" "map_read_target" (func $map_read_target (param i32 i32 i32) (result i32)))
                (import "crml_v1" "map_hide_target" (func $map_hide_target (param i32) (result i32)))
                (import "crml_v1" "map_annotations_publish_v2" (func $annotations_v2 (param i32 i32) (result i32)))
                (import "crml_v1" "map_annotations_publish" (func $annotations_publish (param i32 i32) (result i32)))
                (import "crml_v1" "map_annotations_next" (func $annotations_next (param i32 i32) (result i32)))
                (import "crml_v1" "map_annotations_hide" (func $annotations_hide (result i32)))
                (import "crml_v1" "map_annotations_publish_v3" (func $annotations_v3 (param i32 i32) (result i32)))
                (import "crml_v1" "map_annotations_next_v2" (func $annotations_next_v2 (param i32 i32) (result i32)))
                (import "crml_v1" "map_annotations_status" (func $annotations_status (param i32 i32) (result i32)))
                (import "crml_v1" "capabilities" (func $caps (result i32)))
                (import "crml_v1" "release" (func $release))
                {'(memory (export "memory") 1)' if memory else ''}
                {f'(data (i32.const 0) "{escaped}")' if memory else ''}
                (func $expect (param i32 i32) local.get 0 local.get 1 i32.ne if unreachable end)
                (func (export "crml_abi_version") (result i32) {abi})
                (func (export "crml_init") {init})
                {f'(func (export "crml_tick") (param f32) {tick})' if tick else ''}
                {f'(func (export "crml_shutdown") {shutdown})' if shutdown else ''}
                {f'(func $start {start}) (start $start)' if start else ''})'''
            wat = mod/'mod.wat';wat.write_text(source, encoding='utf-8')
            subprocess.run([str(binary/'crml_wat.exe'), str(wat), str(mod/'mod.wasm')], check=True, capture_output=True, text=True)
            (mod/'mod.ini').write_text(f'id={name}\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\nmin_runtime=0.1.0-alpha.4.4.dev.1\n', encoding='utf-8')
            return mods

        def run(mods, commands, other=None, enabled=True):
            nonlocal cases
            result = subprocess.run([str(binary/'crml_drawing_guest_tests.exe'), str(mods), str(other) if other else '-', str(int(enabled))],
                                    input=commands, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(f'{mods.name}: {result.stdout}\n{result.stderr}')
            cases += 1

        expanded=bytearray(29752)
        struct.pack_into('<4I2Q6f',expanded,0,2,29752,0,2,1,1,0,0,0,0,0,1)
        run(package('annotations-v2',data=expanded,init='i32.const 0 i32.const 29752 call $annotations_v2 i32.const 1 call $expect'), 'active_a 1\n')
        for name,offset,size in [('v2-size',0,29751),('v2-oob',65000,29752)]:
            run(package(name,init=f'i32.const {offset} i32.const {size} call $annotations_v2 drop'),'active_a 0\nfailures_a 1\n')
        map_data = bytearray(6816)
        struct.pack_into('<4IQ2I', map_data, 0, 1, 6816, 1, 600, 1, 1, 0)
        struct.pack_into('<7fI', map_data, 32, 10, 20, 0, 90, 100, 0, .1, 0xffffffff)
        map_publish = 'i32.const 0 i32.const 6816 call $map_publish'
        run(package('map-copied', data=map_data, init=map_publish+' i32.const 1 call $expect i32.const 32 i32.const 0 i32.store',
                    tick='i32.const 7000 i32.const 24 call $map_read i32.const -1 call $expect'),
            'frames 1\ntick_a 501\nframes 0\nactive_a 1\n')
        run(package('map-hide', data=map_data, init=map_publish+' i32.const 1 call $expect call $map_hide i32.const 1 call $expect'), 'frames 0\nactive_a 1\n')
        run(package('map-trap-cleanup', data=map_data, init=map_publish+' i32.const 1 call $expect', tick='unreachable'),
            'frames 1\ntick_a 100\nframes 0\nactive_a 0\n')
        for name, expr in [('map-oob','i32.const 65000 i32.const 6816 call $map_publish'),
                           ('map-size','i32.const 0 i32.const 6815 call $map_publish'),
                           ('map-read-oob','i32.const 65530 i32.const 24 call $map_read'),
                           ('map-target-read-oob','i32.const 2 i32.const 65530 i32.const 24 call $map_read_target'),
                           ('map-target-read-size','i32.const 2 i32.const 7000 i32.const 25 call $map_read_target')]:
            run(package(name, init=expr+' drop'), 'active_a 0\nfailures_a 1\nframes 0\n')

        run(package('map-target-read', init='i32.const 1 i32.const 7000 i32.const 24 call $map_read_target i32.const 1 call $expect '
                    'i32.const 7008 i32.load i32.const 1 call $expect '
                    'i32.const 2 i32.const 7000 i32.const 24 call $map_read_target i32.const -1 call $expect '
                    'i32.const 7008 i32.load i32.const 0 call $expect '
                    'i32.const 3 i32.const 7000 i32.const 24 call $map_read_target i32.const -3 call $expect '
                    'i32.const 3 call $map_hide_target i32.const -3 call $expect '
                    'i32.const 2 call $map_hide_target i32.const 0 call $expect'), 'active_a 1\nfailures_a 0\nframes 0\n')

        for name, expression in [
            ('annotations-size', 'i32.const 0 i32.const 5623 call $annotations_publish'),
            ('annotations-oob', 'i32.const 65500 i32.const 5624 call $annotations_publish'),
            ('annotations-next-size', 'i32.const 0 i32.const 247 call $annotations_next'),
            ('annotations-next-oob', 'i32.const 65500 i32.const 248 call $annotations_next'),
        ]:
            run(package(name, init=expression+' drop'), 'active_a 0\nfailures_a 1\nframes 0\n')
        run(package('annotations-next-empty', init='i32.const 7000 i32.const 248 call $annotations_next i32.const 0 call $expect i32.const 7000 i32.load i32.const 0 call $expect call $annotations_hide i32.const 0 call $expect'), 'active_a 1\nfailures_a 0\nframes 0\n')
        typed=bytearray(31104)
        struct.pack_into('<6I2Q6f',typed,0,3,31104,1,1,6,0,1,1,0,0,0,0,0,1)
        struct.pack_into('<Q3fIIf',typed,64,2,20,100,0,0xffffffff,1,0)
        typed[96:98]=b'W\0';typed[104:110]=b'World\0'
        struct.pack_into('<4I2f',typed,29760,2,1,0xffffffff,0,200,150)
        typed[29784:29786]=b'N\0';typed[29792:29799]=b'Native\0'
        typed_publish='i32.const 0 i32.const 31104 call $annotations_v3'
        typed_status='i32.const 34000 i32.const 32 call $annotations_status'
        run(package('typed-identities',data=typed,init=typed_publish+' i32.const 1 call $expect '+typed_status+
                    ' i32.const 1 call $expect i32.const 34008 i32.load i32.const 6 call $expect '
                    'i32.const 34012 i32.load i32.const 6 call $expect '
                    'i32.const 33000 i32.const 248 call $annotations_next i32.const -3 call $expect '
                    'i32.const 33000 i32.const 488 call $annotations_next_v2 i32.const 0 call $expect '
                    'call $annotations_hide i32.const 1 call $expect '+typed_status+' i32.const 0 call $expect '
                    'i32.const 34000 i32.load i32.const 0 call $expect'), 'active_a 1\nfailures_a 0\nframes 0\n')
        for name,expr in [
            ('typed-size','i32.const 0 i32.const 31103 call $annotations_v3'),
            ('typed-oob','i32.const 40000 i32.const 31104 call $annotations_v3'),
            ('typed-wrap','i32.const -1 i32.const 31104 call $annotations_v3'),
            ('typed-event-size','i32.const 33000 i32.const 487 call $annotations_next_v2'),
            ('typed-event-oob','i32.const 65500 i32.const 488 call $annotations_next_v2'),
            ('typed-status-size','i32.const 34000 i32.const 31 call $annotations_status'),
            ('typed-status-oob','i32.const 65520 i32.const 32 call $annotations_status')]:
            run(package(name,init=expr+' drop'),'active_a 0\nfailures_a 1\nframes 0\n')
        for name,offset,value in [('typed-version',0,2),('typed-count',8,129),('typed-attachments',12,7),
                                  ('typed-reserved',20,1),('typed-world-flags',88,2),('typed-slot',29760,7),('typed-native-reserved',29772,1)]:
            run(package(name,data=typed,init=f'i32.const {offset} i32.const {value} i32.store '+typed_publish+' i32.const -3 call $expect'),
                'active_a 1\nfailures_a 0\nframes 0\n')
        run(package('typed-capacity',data=typed,init='i32.const 8 i32.const 128 i32.store '+typed_publish+' i32.const -3 call $expect'),
            'active_a 1\nfailures_a 0\nframes 0\n')
        run(package('typed-unavailable',data=typed,init=typed_publish+' i32.const -1 call $expect '+typed_status+
                    ' i32.const -1 call $expect i32.const 34000 i32.load i32.const 0 call $expect'),
            'active_a 1\nfailures_a 0\n',enabled=False)
        # Copy happens at the Wasm boundary; subsequent guest stores cannot alter it.
        run(package('copied', init=good+' i32.const 3136 i32.const 88 i32.store8'),
            'active_a 1\nfailures_a 0\nframes 1\nlabel copied\nclock 1000\nframes 0\nshutdown_a\nframes 0\n')
        # Renderer availability appears in the capability intersection.
        run(package('available', init='call $caps i32.const 1048576 i32.and i32.const 1048576 call $expect '+good),
            'active_a 1\nframes 1\n')
        run(package('unavailable', init='call $caps i32.const 1048576 i32.and i32.const 0 call $expect '+publish+' i32.const -1 call $expect'),
            'active_a 1\nfailures_a 0\n', enabled=False)
        run(package('denied', init='', caps='log'), 'active_a 0\nfailures_a 1\nframes 0\n')
        run(package('projection-read',init='i32.const 0 i32.const 64 call $projection i32.const 1 call $expect'),'active_a 1\nfailures_a 0\n')
        for name, init, memory in [
            ('projection-size', 'i32.const 0 i32.const 63 call $projection drop', True),
            ('projection-oob', 'i32.const 65520 i32.const 64 call $projection drop', True),
            ('bad-size', 'i32.const 0 i32.const 5679 call $publish drop', True),
            ('oob', 'i32.const 65500 i32.const 5680 call $publish drop', True),
            ('wrap', 'i32.const -1 i32.const 5680 call $publish drop', True),
            ('missing-memory', publish+' drop', False),
        ]:
            run(package(name, init=init, memory=memory), 'active_a 0\nfailures_a 1\nframes 0\n')
        run(package('invalid', init='i32.const 0 i32.const 2 i32.store '+publish+' i32.const -3 call $expect'),
            'active_a 1\nfailures_a 0\nframes 0\n')
        run(package('limited', init=good+' '+publish+' i32.const -4 call $expect call $hide i32.const 1 call $expect '+publish+' i32.const -4 call $expect'),
            'active_a 1\nfailures_a 0\nframes 0\n')
        run(package('release', tick='call $release'), 'frames 1\ntick_a 100\nframes 0\nactive_a 1\n')
        run(package('tick-trap', tick='unreachable'), 'frames 1\ntick_a 100\nframes 0\nactive_a 0\nfailures_a 1\n')
        run(package('init-trap', init=good+' unreachable'), 'active_a 0\nfailures_a 1\nframes 0\n')
        run(package('version-reject', init='', abi=good+' i32.const 2'), 'active_a 0\nfailures_a 1\nframes 0\n')
        run(package('start-trap', init='', start=good+' unreachable'), 'active_a 0\nfailures_a 1\nframes 0\n')
        run(package('shutdown-publish', shutdown=good), 'frames 1\nclock 100\nshutdown_a\nframes 0\nfailures_a 0\n')
        run(package('shutdown-trap', shutdown='unreachable'), 'frames 1\nshutdown_a\nframes 0\nfailures_a 1\n')
        # Hide and publish share the eight-command allowance with release.
        run(package('budget', tick='call $release '*8+' call $hide drop'),
            'frames 1\ntick_a 100\nframes 0\nactive_a 0\nfailures_a 1\n')
        a=package('isolate-a', tick='call $release');b=package('isolate-b')
        run(a, 'frames 2\ntick_a 100\nframes 1\nlabel isolate-b\nshutdown_a\nframes 1\nshutdown_b\nframes 0\n', b)
        route_mods=work/'compiled-route'
        route_package=route_mods/'route-sketch';route_package.mkdir(parents=True)
        sys.path.insert(0,str(root/'tools'))
        import mod as mod_tools
        # Same freestanding SDK build flags, with an inherited-ACL output path.
        # Package/build helper workflow is covered separately by SDK tests.
        subprocess.run([str(mod_tools.compiler(None)),'--target=wasm32-unknown-unknown','-std=c11','-O2',
                        '-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-nostdlib',
                        '-I',str(root/'sdk/include'),str(root/'examples/route-sketch/route-sketch.c'),
                        '-Wl,--no-entry','-Wl,--export-memory','-Wl,--export=crml_abi_version',
                        '-Wl,--export=crml_init','-Wl,--export-if-defined=crml_tick','-Wl,--export-if-defined=crml_shutdown',
                        '-Wl,--max-memory=16777216','-o',str(route_package/'route-sketch.wasm')],check=True)
        shutil.copyfile(root/'examples/route-sketch/mod.ini',route_package/'mod.ini')
        subprocess.run([str(binary/'crml_drawing_guest_tests.exe'),str(route_mods),'--route'],check=True)
        print(f'PASS: {cases} real Wasm drawing boundary/lifecycle/isolation scenarios')


if __name__ == '__main__':
    main()
