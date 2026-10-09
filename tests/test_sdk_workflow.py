"""Compile maintained C examples and exercise the supported author commands."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]

# Release acceptance checks an explicit deliverable set. Discovering only files
# present in an archive lets missing packages or scenarios silently pass.
MAINTAINED_EXAMPLES = {
    'area-actions': (),
    'hello': (),
    'input-actions': (),
    'movement': (),
    'photo-visibility': ('hold-recovery', 'lease-refusal', 'long-stall', 'settings-toggle'),
    'physics-damping': (),
    'settings': (),
    'startup-preferences': ('dispatched-visit', 'long-stall', 'movie-no-replay',
                            'settings-behavior', 'skipped-bounded', 'unknown-no-replay'),
    'startup-skip': (),
    'state-watch': (),
    'visibility': (),
    'tutorials': (),
    'route-sketch': (),
    'list-browser': (),
}


def required_examples(sdk):
    required = ['sdk/scenarios/movement-recovery.json', 'sdk/scenarios/composed-author-services.json']
    for name, scenarios in MAINTAINED_EXAMPLES.items():
        required += [f'examples/{name}/{name}.c', f'examples/{name}/mod.ini']
        required += [f'examples/{name}/{scenario}.json' for scenario in scenarios]
    missing = [name for name in required if not (sdk/name).is_file()]
    if missing:
        raise ValueError('SDK is missing maintained example inputs: '+', '.join(missing))
    discovered = {path.name for path in (sdk/'examples').iterdir() if list(path.glob('*.c'))}
    if discovered != set(MAINTAINED_EXAMPLES):
        raise ValueError('Update the maintained example acceptance inventory: '+', '.join(sorted(discovered-set(MAINTAINED_EXAMPLES))))
    return [sdk/'examples'/name for name in MAINTAINED_EXAMPLES]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=ROOT)
    parser.add_argument('--archive', type=Path, help='Extract and test an actual packaged SDK ZIP')
    parser.add_argument('--bin', type=Path)
    parser.add_argument('--clang', type=Path)
    args = parser.parse_args()
    # Keep the extraction directory alive until all compiler/host processes exit.
    with tempfile.TemporaryDirectory(prefix='crml-sdk-archive-') as extraction:
        if args.archive:
            destination = Path(extraction).resolve()
            with zipfile.ZipFile(args.archive) as archive:
                for name in archive.namelist():
                    if not (destination/name).resolve().is_relative_to(destination):
                        raise ValueError('Unsafe SDK archive path')
                archive.extractall(destination)
            args.sdk = destination
        exercise(args)


def exercise(args):
    sdk = args.sdk.resolve()
    examples = required_examples(sdk)
    spec = importlib.util.spec_from_file_location('crml_mod', sdk/'tools/mod.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    clang = mod.compiler(args.clang)
    host = args.bin.resolve()/'crml_host.exe' if args.bin else mod.tool('crml_host')
    with tempfile.TemporaryDirectory(prefix='crml-sdk-') as temporary:
        root = Path(temporary)
        source, built = root/'source', root/'built'
        mod.create(source, 'independent-author')
        mod.build(source, built, clang)
        mod.check(built, host, 2)
        # Run the CLI from an unrelated cwd, as users do with an extracted SDK.
        subprocess.run([sys.executable, str(sdk/'tools/mod.py'), 'check', str(built), '--host', str(host)],
                       cwd=root, check=True)
        try:
            mod.create(source, 'other')
        except ValueError:
            pass
        else:
            raise AssertionError('new must not overwrite an existing source tree')
        storage_source=root/'storage-source'
        mod.create(storage_source,'storage-fixture')
        (storage_source/'mod.ini').write_text(
            'id=storage-fixture\nabi=1\nmodule=mod.wasm\ncapabilities=log,storage\n'
            'min_runtime=0.1.0-alpha.4.3.dev.0\n',encoding='utf-8')
        (storage_source/'main.c').write_text('''#include "crml.h"
CRML_EXPORT("crml_abi_version") uint32_t crml_abi_version(void) { return 1; }
CRML_EXPORT("crml_init") void crml_init(void) {
    unsigned char record[4] = {0};
    if (!(crml_capabilities() & CRML_CAP_STORAGE)) __builtin_trap();
    int result = crml_storage_read(record, sizeof(record));
    if (result == -2) {
        const unsigned char initial[4] = {1,3,9,7};
        if (crml_storage_write(initial, sizeof(initial)) != 0) __builtin_trap();
        int status = crml_storage_status();
        if (status != 1 && status != 2) __builtin_trap();
        crml_log(1, "queued", 6);
    } else {
        if (result != 4 || record[0] != 1 || record[1] != 3 || record[2] != 9 || record[3] != 7) __builtin_trap();
        crml_log(1, "restored", 8);
    }
}
''',encoding='utf-8')
        storage_mods=root/'storage-mods'
        mod.build(storage_source,storage_mods/'fixture',clang)
        for expected in ('queued','restored'):
            result=subprocess.run([str(host),str(storage_mods),'0'],capture_output=True,text=True,timeout=15,check=True)
            assert expected in result.stdout and 'failures: 0' in result.stdout,result.stdout
        bindings_source=root/'bindings-source'
        mod.create(bindings_source,'bindings-fixture')
        (bindings_source/'mod.ini').write_text(
            'id=bindings-fixture\nabi=1\nmodule=mod.wasm\ncapabilities=input.actions\n'
            'min_runtime=0.1.0-alpha.4.3.dev.0\naction.15=F11\n',encoding='utf-8')
        (bindings_source/'main.c').write_text('''#include "crml.h"
uint32_t crml_abi_version(void) { return 1; }
void crml_init(void) {
    for (unsigned slot=0; slot<16; ++slot) {
        const int result=crml_input_bind(slot, slot ? "None" : "N", slot ? 4 : 1);
        if (result<0) __builtin_trap();
    }
    crml_input_state state;
    if (crml_input_read(&state, sizeof(state))!=1 || state.version!=1 || state.size!=232 ||
        state.flags || state.held || state.bound!=1 || state.shared || state.duplicate ||
        state.host_shortcut || state.binding_revision!=3 || state.names[0][0]!='N' ||
        state.names[0][1] || state.names[15][0]!='N' || state.names[15][1]!='o')
        __builtin_trap();
}
''',encoding='utf-8')
        bindings_mods=root/'bindings-mods'
        mod.build(bindings_source,bindings_mods/'fixture',clang)
        mod.check(bindings_mods/'fixture',host,2)
        ui_source=root/'ui-source'
        mod.create(ui_source,'ui-receipt-fixture')
        (ui_source/'mod.ini').write_text(
            'id=ui-receipt-fixture\nabi=1\nmodule=mod.wasm\ncapabilities=ui.read,ui.activate\n'
            'min_runtime=0.1.0-alpha.4.3.dev.0\n',encoding='utf-8')
        (ui_source/'main.c').write_text('''#include "crml.h"
static int64_t ticket;
uint32_t crml_abi_version(void) { return 1; }
void crml_init(void) {
    crml_ui_state state;
    if(crml_ui_read(&state,sizeof(state))!=1) __builtin_trap();
    ticket=crml_ui_action_submit(state.generation,CRML_UI_ACTION_CONTINUE);
    if(ticket<=0 || crml_ui_action_status(ticket)!=CRML_UI_ACTION_QUEUED) __builtin_trap();
}
void crml_tick(float dt) {
    (void)dt;
    if(crml_ui_action_status(ticket)!=CRML_UI_ACTION_DISPATCHED) __builtin_trap();
}
''',encoding='utf-8')
        ui_mods=root/'ui-mods'
        mod.build(ui_source,ui_mods/'fixture',clang)
        ui_scenario=root/'ui-scenario.json'
        ui_scenario.write_text(json.dumps({'schema':1,'capabilities':['ui.read','ui.activate'],
            'initial':{'ui':{'screen':1,'actions':1}},'frames':[{'dt_ms':10}],
            'expect':[{'op':'ui_action_submit','result':1,'args':[1,1]}]}),encoding='utf-8')
        mod.simulate(ui_mods,ui_scenario,host,root/'ui-report.json')
        for source in examples:
            output = root/source.name
            mod.build(source, output, clang)
            mod.check(output, host, 10)
            for scenario in sorted(source.glob('*.json')):
                scenario_mods = root/('scenario-'+source.name)
                scenario_mods.mkdir(exist_ok=True)
                shutil.copytree(output, scenario_mods/source.name, dirs_exist_ok=True)
                mod.simulate(scenario_mods, scenario, host, root/(source.name+'-'+scenario.stem+'-report.json'))
            if source.name == 'movement':
                scenario_mods = root/'scenario-mods'
                shutil.copytree(output, scenario_mods/'movement')
                mod.simulate(scenario_mods, sdk/'sdk/scenarios/movement-recovery.json', host, root/'scenario-report.json')
                profiled = subprocess.run([sys.executable, str(sdk/'tools/mod.py'), 'simulate',
                    str(scenario_mods), str(sdk/'sdk/scenarios/movement-recovery.json'),
                    '--host', str(host), '--profile'], cwd=root, capture_output=True,
                    text=True, encoding='utf-8', timeout=60)
                if profiled.returncode or '[host] Metrics movement phase=tick ' not in profiled.stdout:
                    raise AssertionError('Profile CLI did not display measured callback costs: '+profiled.stdout+profiled.stderr)
            # Preserve the established WAT fixtures as a behavior comparison
            # while switching shipped examples to C. This is a native fake
            # provider, not evidence that either version works in the game.
            gameplay = host.parent/'crml_gameplay_tests.exe'
            wat = source/(source.name+'.wat')
            # Movement C deliberately adds context-aware recovery which the
            # historical WAT fixture lacks; the composed scenario covers it.
            if gameplay.is_file() and wat.is_file() and source.name != 'movement':
                reference = root/(source.name+'-wat')
                reference.mkdir()
                shutil.copyfile(source/'mod.ini', reference/'mod.ini')
                module = mod.manifest(source/'mod.ini')['module']
                mod.run([host.parent/'crml_wat.exe', wat, reference/module])
                modes = ['4']
                if source.name == 'startup-skip':
                    modes += ['ui-screen-'+str(i) for i in range(10)]
                    modes += ['ui-changes', 'ui-generation', 'ui-transient', 'ui-no-actions',
                              'ui-present-release-rejected', 'media-generation']
                for mode in modes:
                    observed = []
                    for package in (output, reference):
                        mods = root/'compare'
                        mods.mkdir(exist_ok=True)
                        shutil.copytree(package, mods/'mod', dirs_exist_ok=True)
                        result = subprocess.run([str(gameplay), str(mods), mode], check=True,
                                                capture_output=True, text=True, timeout=15)
                        assert 'failures: 0; owners: 0' in result.stdout, result.stdout
                        observed.append(result.stdout)
                    assert observed[0] == observed[1], f'C/WAT behavior mismatch: {source.name} {mode}\n{observed}'
        composed = root/'composed-mods'
        for name in ('movement', 'photo-visibility', 'startup-preferences'):
            shutil.copytree(root/name, composed/name)
        result = mod.simulate(composed, sdk/'sdk/scenarios/composed-author-services.json',
                              host, root/'composed-report.json')
        for operation in ('motion_set', 'visibility_set', 'ui_action_submit', 'ui_present', 'media_skip'):
            assert any(event['op'] == operation and event['result'] >= 0 for event in result['events']), operation
        for frame in (8, 15, 21, 27):
            for operation in ('motion_set', 'visibility_set'):
                assert any(event['frame'] == frame and event['op'] == operation and event['result'] == 1
                           and event['args'][0] == 1 for event in result['events']), (frame, operation)
        for first, last in ((10, 13), (17, 19), (23, 25)):
            assert not any(first <= event['frame'] <= last and event['op'] in ('motion_set', 'visibility_set')
                           and event['result'] == 1 and event['args'][0] == 1
                           for event in result['events']), 'Composed movement/visibility rearmed during recovery'
        # Bad guest C must preserve the last successful package binary.
        source = root/'source'
        previous = (built/'mod.wasm').read_bytes()
        (source/'main.c').write_text('#error deliberate failed build\n', encoding='utf-8')
        try:
            mod.build(source, built, clang)
        except subprocess.CalledProcessError:
            pass
        else:
            raise AssertionError('invalid C accepted')
        assert (built/'mod.wasm').read_bytes() == previous
        print('C template, storage restart, live bindings, maintained examples, independent cwd and failed-build preservation passed.')


if __name__ == '__main__':
    main()
