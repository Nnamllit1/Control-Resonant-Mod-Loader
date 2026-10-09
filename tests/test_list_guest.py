"""Real Wasmtime boundary, copied payload, revision and lifecycle tests for lists."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import uuid


PAGE_SIZE = 9840
ROW = 0x100000002


def descriptor(title):
    data = bytearray(PAGE_SIZE)
    struct.pack_into('<IIIf', data, 0, 1, PAGE_SIZE, 2, 1.0)
    data[16:16+len(title)] = title.encode('utf-8')
    for index, (identifier, flags, label) in enumerate([(ROW, 3, 'Enabled'), (7, 0, 'Disabled')]):
        at = 112 + 304*index
        struct.pack_into('<QII', data, at, identifier, flags, 0)
        data[at+16:at+16+len(label)] = label.encode('utf-8')
    return data


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin', type=Path, required=True)
    args = parser.parse_args()
    binary = args.bin.resolve()
    root = Path(__file__).resolve().parents[1]
    work = root/'.local'/'test-tmp'/('list-guest-'+uuid.uuid4().hex)
    work.mkdir(parents=True)
    cases = 0

    def package(name, init='call $publishgood', tick='', shutdown='', caps='lists', memory=True,
                abi='i32.const 1', start='', offset=0, event=20000, data=None,
                minimum='0.1.0-alpha.4.4.dev.1', extra=''):
        mods = work/name
        mod = mods/name
        mod.mkdir(parents=True)
        body = descriptor(name) if data is None else data
        escaped = ''.join(f'\\{b:02x}' for b in body)
        source = f'''(module
            (import "crml_v1" "list_publish" (func $publish (param i32 i32) (result i64)))
            (import "crml_v1" "list_next" (func $next (param i32 i32) (result i32)))
            (import "crml_v1" "list_hide" (func $hide (result i32)))
            (import "crml_v1" "capabilities" (func $caps (result i32)))
            (import "crml_v1" "release" (func $release))
            {'(memory (export "memory") 1)' if memory else ''}
            {extra}
            {f'(data (i32.const {offset}) "{escaped}")' if memory else ''}
            (global $revision (mut i64) (i64.const 0))
            (global $sequence (mut i64) (i64.const 0))
            (func $expect (param i32 i32) local.get 0 local.get 1 i32.ne if unreachable end)
            (func $expect64 (param i64 i64) local.get 0 local.get 1 i64.ne if unreachable end)
            (func $publishgood
                i32.const {offset} i32.const {PAGE_SIZE} call $publish global.set $revision
                global.get $revision i64.const 0 i64.le_s if unreachable end)
            (func $dirty
                {' '.join(f'i32.const {event+i} i64.const -1 i64.store' for i in (0,8,16,24)) if memory else ''})
            (func $zero
                {' '.join(f'i32.const {event+i} i64.load i64.const 0 call $expect64' for i in (0,8,16,24)) if memory else ''})
            (func $empty (param $result i32)
                call $dirty i32.const {event} i32.const 32 call $next local.get $result call $expect call $zero)
            (func $read
                i32.const {event} i32.const 32 call $next i32.const 1 call $expect
                {' '.join([f'i32.const {event} i32.load i32.const 1 call $expect',f'i32.const {event+4} i32.load i32.const 0 call $expect', 'global.get $sequence i64.const 1 i64.add global.set $sequence',f'i32.const {event+8} i64.load global.get $sequence call $expect64',f'i32.const {event+16} i64.load global.get $revision call $expect64',f'i32.const {event+24} i64.load i64.const {ROW} call $expect64']) if memory else ''})
            (func (export "crml_abi_version") (result i32) {abi})
            (func (export "crml_init") {init})
            {f'(func (export "crml_tick") (param f32) {tick})' if tick else ''}
            {f'(func (export "crml_shutdown") {shutdown})' if shutdown else ''}
            {f'(func $start {start}) (start $start)' if start else ''})'''
        wat = mod/'mod.wat';wat.write_text(source, encoding='utf-8')
        result = subprocess.run([str(binary/'crml_wat.exe'), str(wat), str(mod/'mod.wasm')], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(f'{name}: invalid WAT: {result.stderr}')
        (mod/'mod.ini').write_text(f'id={name}\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\nmin_runtime={minimum}\n', encoding='utf-8')
        return mods

    def run(mods, commands, other=None, enabled=True):
        nonlocal cases
        result = subprocess.run([str(binary/'crml_list_guest_tests.exe'), str(mods), str(other) if other else '-', str(int(enabled))],
                                input=commands, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(f'{mods.name}: {result.stdout}\n{result.stderr}')
        cases += 1

    ok = 'active_a 1\nfailures_a 0\n'
    rejected = 'active_a 0\nfailures_a 1\npages 0\n'
    run(package('copied', init='call $publishgood i32.const 16 i32.const 88 i32.store8 i32.const 128 i32.const 88 i32.store8'),
        ok+'pages 1\ntitle copied\nlabel Enabled\nshutdown_a\npages 0\n')
    run(package('available', init='call $caps i32.const 2097152 i32.and i32.const 2097152 call $expect call $publishgood'), ok+'pages 1\n')
    run(package('headless', init='call $caps i32.const 2097152 i32.and i32.const 0 call $expect i32.const 0 i32.const 9840 call $publish i64.const -1 call $expect64 i32.const -1 call $empty'),ok,enabled=False)
    run(package('no-owner', tick='i32.const -1 call $empty i32.const 0 i32.const 9840 call $publish i64.const -1 call $expect64 call $hide i32.const -1 call $expect'),
        'save old no-owner\ndetach old\ntick_a 100\n'+ok+'pages 0\n')
    run(package('denied', caps='log', init=''),rejected)
    for name, init, memory in [
        ('publish-short','i32.const 0 i32.const 9839 call $publish drop',True),
        ('publish-large','i32.const 0 i32.const 9841 call $publish drop',True),
        ('publish-oob','i32.const 55697 i32.const 9840 call $publish drop',True),
        ('publish-wrap','i32.const -1 i32.const 9840 call $publish drop',True),
        ('next-short','i32.const 20000 i32.const 31 call $next drop',True),
        ('next-large','i32.const 20000 i32.const 33 call $next drop',True),
        ('next-oob','i32.const 65505 i32.const 32 call $next drop',True),
        ('next-wrap','i32.const -1 i32.const 32 call $next drop',True),
        ('missing-memory','i32.const 0 i32.const 9840 call $publish drop',False),
        ('next-missing-memory','i32.const 0 i32.const 32 call $next drop',False),
    ]:
        run(package(name,init=init,memory=memory),rejected)
    run(package('wrong-memory',init='i32.const 0 i32.const 32 call $next drop',memory=False,extra='(global (export "memory") i32 (i32.const 0))'),rejected)
    run(package('last-byte',offset=55696,event=65504,init='call $publishgood i32.const 0 call $empty'),ok+'pages 1\n')
    run(package('unaligned',offset=1,event=20001,init='call $publishgood i32.const 0 call $empty'),ok+'pages 1\n')
    run(package('invalid',init='i32.const 0 i32.const 2 i32.store i32.const 0 i32.const 9840 call $publish i64.const -3 call $expect64'),ok+'pages 0\n')
    run(package('rate',init='call $publishgood i32.const 0 i32.const 9840 call $publish i64.const -4 call $expect64 call $hide i32.const 1 call $expect i32.const 0 i32.const 9840 call $publish i64.const -4 call $expect64'),ok+'pages 0\n')
    run(package('actions',tick='call $read i32.const 0 call $empty'),
        f'save old actions\nactivate old 7 0 409\nactivate old 99 0 404\nactivate old {ROW} 1 409\nactivate old {ROW} 0 200\ntick_a 100\n'+ok)
    run(package('full',tick='call $read '*8),
        f'save old full\nfill old {ROW} 0 200 16\nactivate old {ROW} 0 429\ntick_a 100\ntick_a 100\n'+ok)
    # Replacement changes revision but already accepted events retain the old one.
    run(package('old-revision',tick='i32.const 16 i32.const 88 i32.store8 i32.const 0 i32.const 9840 call $publish global.get $revision i64.le_u if unreachable end call $read'),
        f'save old old-revision\nactivate old {ROW} 0 200\ntick_a 100\nactivate old {ROW} 0 409\n'+ok)
    run(package('same-revision',tick='i32.const 0 i32.const 9840 call $publish global.get $revision call $expect64 call $read'),
        f'save old same-revision\nactivate old {ROW} 0 200\ntick_a 100\nactivate old {ROW} 0 200\n'+ok)
    run(package('renderer-clear',tick='i32.const -1 call $empty'),
        f'save old renderer-clear\nactivate old {ROW} 0 200\nrenderer 0\ntick_a 100\nrenderer 1\npages 0\nactivate old {ROW} 0 409\n'+ok)
    run(package('release',tick='call $release i32.const 0 call $empty'),
        f'save old release\nactivate old {ROW} 0 200\ntick_a 100\npages 0\nactivate old {ROW} 0 409\n'+ok)
    run(package('tick-trap',tick='unreachable'),'pages 1\ntick_a 100\n'+rejected)
    run(package('init-trap',init='call $publishgood unreachable'),rejected)
    run(package('version-reject',init='',abi='call $publishgood i32.const 2'),rejected)
    run(package('start-trap',init='',start='call $publishgood unreachable'),rejected)
    run(package('shutdown-publish',shutdown='call $publishgood'),'pages 1\nclock 100\nshutdown_a\npages 0\nfailures_a 0\n')
    run(package('shutdown-trap',shutdown='unreachable'),'pages 1\nshutdown_a\n'+rejected)
    run(package('command-budget',tick='call $release '*8+' call $hide drop'),'pages 1\ntick_a 100\n'+rejected)
    run(package('read-budget',tick='i32.const 0 call $empty '*9),'pages 1\ntick_a 100\n'+rejected)
    a=package('isolate-a',tick='call $release');b=package('isolate-b',tick='call $read')
    run(a,f'pages 2\nsave b isolate-b\nactivate b {ROW} 0 200\ntick_a 100\npages 1\ntitle isolate-b\ntick_b 100\nactive_b 1\nfailures_b 0\nshutdown_a\npages 1\nshutdown_b\npages 0\n',b)
    run(package('reload',tick='i32.const 0 call $empty'),f'save old reload\nactivate old {ROW} 0 200\nclock 100\nshutdown_a\nload_b_from_a\npages 1\nactivate old {ROW} 0 404\ntick_b 100\nactive_b 1\nfailures_b 0\n')
    compiled=work/'compiled-browser'
    # Exercise the published SDK build command, then the resulting C guest.
    subprocess.run([sys.executable,str(root/'tools/mod.py'),'build',str(root/'examples/list-browser'),
                    '--output',str(compiled/'list-browser')],check=True)
    subprocess.run([str(binary/'crml_list_guest_tests.exe'),str(compiled),'--browser'],check=True)
    print(f'PASS: {cases} real Wasm list boundary/revision/lifecycle/isolation scenarios')


if __name__ == '__main__':
    main()
