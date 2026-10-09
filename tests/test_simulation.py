"""Scripted guest behavior through the real Wasmtime sandbox; no game process."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('crml_mod', ROOT/'tools/mod.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
BIN = None
ABI = '(func (export "crml_abi_version") (result i32) i32.const 1)'


class SimulationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.mods = self.root/'mods'
        self.mods.mkdir()

    def guest(self, body, capabilities, name='test', bindings='', abi=ABI):
        folder = self.mods/name
        folder.mkdir()
        (folder/'mod.ini').write_text(f'id={name}\nabi=1\nmodule=mod.wasm\ncapabilities={capabilities}\n{bindings}', encoding='utf-8')
        source = self.root/(name+'.wat')
        source.write_text(f'(module {body} {abi})', encoding='utf-8')
        subprocess.run([str(BIN/'crml_wat.exe'), str(source), str(folder/'mod.wasm')], check=True)

    def simulate(self, document):
        scenario = self.root/'scenario.json'
        scenario.write_text(json.dumps(document), encoding='utf-8')
        return mod.simulate(self.mods, scenario, BIN/'crml_host.exe', self.root/'report.json')

    def c_guest(self, code, capabilities):
        source = self.root/'source'
        source.mkdir()
        (source/'main.c').write_text('#include "crml.h"\nuint32_t crml_abi_version(void){return 1;}\n'+code, encoding='utf-8')
        (source/'mod.ini').write_text(f'id=test\nabi=1\nmodule=mod.wasm\ncapabilities={capabilities}\n', encoding='utf-8')
        mod.build(source, self.mods/'test')

    def test_movement_recovery_example(self):
        mod.build(ROOT/'examples/movement', self.mods/'movement')
        document = json.loads((ROOT/'sdk/scenarios/movement-recovery.json').read_text())
        first = self.simulate(document)
        second = self.simulate(document)
        self.assertEqual(first, second)

    def test_composed_author_mods_keep_budgets_and_recovery_in_both_orders(self):
        # Real maintained guests and host services. No native rendering/storage
        # evidence is implied by the scripted observations in this scenario.
        names = ['movement', 'photo-visibility', 'startup-preferences']
        traces = []
        scenario = ROOT/'sdk/scenarios/composed-author-services.json'
        document = json.loads(scenario.read_text(encoding='utf-8'))
        for order in (names, list(reversed(names))):
            packages = self.root/('forward' if order == names else 'reverse')
            for index, name in enumerate(order):
                mod.build(ROOT/'examples'/name, packages/(str(index)+'-'+name))
            result = mod.simulate(packages, scenario, BIN/'crml_host.exe', profile=True)
            events = result['events']
            for frame in (8, 15, 21, 27):
                for operation in ('motion_set', 'visibility_set'):
                    self.assertTrue(any(event['frame'] == frame and event['op'] == operation
                                        and event['result'] == 1 and event['args'][0] == 1
                                        for event in events), (order, frame, operation))
            # The first focus loss, held-key recovery and long stall must leave
            # both operations stopped until the scenario releases/represses keys.
            for first, last in ((10, 13), (17, 19), (23, 25)):
                renewed = [event for event in events if first <= event['frame'] <= last
                           and event['op'] in ('motion_set', 'visibility_set')
                           and event['result'] == 1 and event['args'][0] == 1]
                self.assertEqual(renewed, [], (order, first, last))
            for operation in ('ui_action_submit', 'ui_present', 'media_skip',
                              'motion_set', 'visibility_set'):
                self.assertTrue(any(event['op'] == operation and event['result'] >= 0
                                    for event in events), (order, operation))
            ticks = {}
            for line in result['logs']:
                match = re.search(r'\[host\] Metrics (\S+) phase=tick (.*)', line)
                if match:
                    ticks[match[1]] = dict(part.split('=') for part in match[2].split())
            self.assertEqual(set(ticks), set(names))
            for name, stats in ticks.items():
                self.assertEqual(int(stats['calls']), len(document['frames']), name)
                self.assertEqual(int(stats['failures']), 0, name)
                self.assertEqual(int(stats['fuel_errors']), 0, name)
                self.assertLessEqual(int(stats['reads_peak']), 8, name)
                self.assertLessEqual(int(stats['commands_peak']), 8, name)
                self.assertLess(int(stats['fuel_peak']), 100000, name)
            # Package order changes owner numbers and callback event ordering,
            # but not these guests' resource-scoped behavior within each frame.
            normalized = []
            for event in events:
                event = dict(event)
                event['owner'] = order[event['owner']-1] if event['owner'] else 'host'
                normalized.append(json.dumps(event, sort_keys=True))
            traces.append(sorted(normalized))
        self.assertEqual(*traces)

    def test_clock_is_elapsed_time_not_clamped_delta(self):
        self.c_guest('''
static unsigned step;
void crml_init(void) {if(crml_clock_ms()!=0) __builtin_trap();}
void crml_tick(float dt) {
    const uint64_t expected[]={17,60017,60017,60020};
    const float expected_dt[]={0.017f,1.0f,0.0f,0.003f};
    for(unsigned i=0;i<8;++i) if(crml_clock_ms()!=expected[step]) __builtin_trap();
    if(dt!=expected_dt[step]) __builtin_trap();
    ++step;
}
void crml_shutdown(void) {if(crml_clock_ms()!=60020) __builtin_trap();}
''','')
        self.simulate({'schema':1,'capabilities':[],'frames':[{'dt_ms':17},{'dt_ms':60000},{'dt_ms':0},{'dt_ms':3}],'expect':[]})

    def test_clock_uses_observation_budget(self):
        self.c_guest('''
void crml_init(void) {for(unsigned i=0;i<8;++i) crml_clock_ms();}
void crml_tick(float dt) {(void)dt;for(unsigned i=0;i<9;++i) crml_clock_ms();}
''','')
        result=self.simulate({'schema':1,'frames':[{}],'expect_failures':1,'expect':[]})
        self.assertTrue(any('Observation call budget exceeded' in line for line in result['logs']))

    def test_clock_budget_and_timestamp_cover_every_guest_invocation(self):
        self.guest('''
(import "crml_v1" "clock_ms" (func $clock (result i64)))
(global $stage (mut i32) (i32.const 0))
(func $check (param $stage i32) (local $i i32) (local $expected i64)
  local.get $stage global.get $stage i32.ne if unreachable end
  local.get $stage i32.const 3 i32.ge_u if i64.const 10 local.set $expected end
  (loop $again
    call $clock local.get $expected i64.ne if unreachable end
    local.get $i i32.const 1 i32.add local.tee $i i32.const 8 i32.lt_u br_if $again)
  global.get $stage i32.const 1 i32.add global.set $stage)
(func $start i32.const 0 call $check) (start $start)
(func (export "crml_init") i32.const 2 call $check)
(func (export "crml_tick") (param f32) i32.const 3 call $check)
(func (export "crml_shutdown") i32.const 4 call $check)
''','',abi='(func (export "crml_abi_version") (result i32) i32.const 1 call $check i32.const 1)')
        self.simulate({'schema':1,'frames':[{'dt_ms':10}],'expect':[]})

    def test_text_setting_edits_use_real_registry(self):
        self.c_guest('''
static unsigned step;
void crml_init(void) {
    crml_text_setting_definition d={1,16,"name","Name","","default"};
    if(crml_settings_text_register(&d,sizeof(d))!=1) __builtin_trap();
}
void crml_tick(float dt) {
    (void)dt; crml_text_setting_value v;
    if(crml_settings_text_read(1,&v,sizeof(v))!=1 || v.revision!=(step?3:2)) __builtin_trap();
    if(step?(v.length!=0):(v.length!=7 || v.value[0]!='A' || v.value[1]!=' ' || (unsigned char)v.value[2]!=0xc3)) __builtin_trap();
    ++step;
}
''','settings')
        edit=lambda v:{'mod':'test','key':'name','value':v}
        self.simulate({'schema':1,'initial':{'settings':[edit('A é /!')]},
                       'frames':[{}, {'state':{'settings':[edit('')]}}],'expect':[]})
        for bad in [edit('x'*17), edit(1), dict(edit('text'),mod='missing'), dict(edit('text'),key='missing')]:
            with self.assertRaises((ValueError, RuntimeError)):
                self.simulate({'schema':1,'initial':{'settings':[bad]},'frames':[],'expect':[]})
        for bad in ['\ud800','\n','\u0085','\u2028','x'*256]:
            with self.assertRaises((ValueError, UnicodeError)):
                self.simulate({'schema':1,'initial':{'settings':[edit(bad)]},'frames':[],'expect':[]})

    def test_setting_edits_use_real_owner_definitions(self):
        self.c_guest('''
static unsigned step;
void crml_init(void) {
    crml_setting_definition d={1,CRML_SETTING_INT,"count","Count","",0,0,5,1};
    if(crml_settings_register(&d,sizeof(d))!=1) __builtin_trap();
}
void crml_tick(float dt) {
    (void)dt; crml_setting_value v;
    if(crml_settings_read(&v,sizeof(v))!=1 || v.value!=(step?4:2) || v.revision!=(step?3:2)) __builtin_trap();
    ++step;
}
''','settings')
        edit=lambda v:{'mod':'test','key':'count','value':v}
        self.simulate({'schema':1,'initial':{'settings':[edit(2)]},
                      'frames':[{}, {'state':{'settings':[edit(4)]}}],'expect':[]})
        for bad in [dict(edit(1),mod='missing'), dict(edit(1),key='missing'), edit(0.5), edit(6)]:
            with self.subTest(edit=bad), self.assertRaisesRegex(ValueError,'Simulation host failed'):
                self.simulate({'schema':1,'initial':{'settings':[bad]},'frames':[]})

    def test_media_consumption_and_retirement_use_native_service(self):
        self.c_guest('''
static unsigned step;
void crml_init(void) {}
void crml_tick(float dt) {
    (void)dt; crml_media_state s;
    int r=crml_media_read(&s,sizeof(s));
    if(step==0) {
        if(r!=1 || s.generation!=1 || !(s.flags&CRML_MEDIA_SKIPPABLE) || crml_media_skip(s.generation)!=0) __builtin_trap();
    } else if(step==1) {
        if(r!=1 || crml_media_skip(s.generation)!=-1) __builtin_trap();
    } else if(r!=-1) __builtin_trap();
    ++step;
}
''','media.read,media.skip')
        self.simulate({'schema':1,'initial':{'media':{'elapsed_ms':2500,'name':'data/boot.tex'},'media_consume':False},
                      'frames':[{}, {}, {'state':{'media_consume':True}}, {}],
                      'expect':[{'op':'media_skip','result':0,'args':[1]},
                                {'op':'media_skip','result':-1,'args':[1]},
                                {'op':'media_consumed','owner':0,'args':[1]}]})

    def test_ui_receipts_dispatch_skip_failure_and_trace(self):
        self.c_guest('''
static int64_t receipt;
static unsigned step;
static void submit(void) {
    crml_ui_state state;
    if (crml_ui_read(&state,sizeof(state)) != 1 || !state.generation) __builtin_trap();
    receipt = crml_ui_action_submit(state.generation, CRML_UI_ACTION_CONTINUE);
    if (receipt <= 0 || crml_ui_action_status(receipt) != CRML_UI_ACTION_QUEUED) __builtin_trap();
}
void crml_init(void) { submit(); }
void crml_tick(float dt) {
    (void)dt;
    const int expected[] = {CRML_UI_ACTION_DISPATCHED,CRML_UI_ACTION_SKIPPED,CRML_UI_ACTION_DISPATCH_FAILED};
    if (crml_ui_action_status(receipt) != expected[step++]) __builtin_trap();
    if (step < 3) submit();
}
''', 'ui.read,ui.activate')
        document={'schema':1,'capabilities':['ui.read','ui.activate'],
            'initial':{'ui':{'screen':1,'actions':1}},'frames':[
                {'dt_ms':10,'state':{'ui_outcome':'skipped'}},
                {'dt_ms':10,'state':{'ui_outcome':'failed'}},{'dt_ms':10}],
            'expect':[{'op':'ui_action_submit','result':i+1,'args':[1,1]} for i in range(3)]}
        self.assertEqual(self.simulate(document), self.simulate(document))

    def test_ui_receipts_expiry_and_page_replacement(self):
        self.c_guest('''
static int64_t receipt;
static unsigned step;
static void submit(void) {
    crml_ui_state state;
    if (crml_ui_read(&state,sizeof(state)) != 1) __builtin_trap();
    receipt = crml_ui_action_submit(state.generation, CRML_UI_ACTION_CONTINUE);
    if (receipt <= 0) __builtin_trap();
}
void crml_init(void) { submit(); }
void crml_tick(float dt) {
    (void)dt;
    const int expected[] = {CRML_UI_ACTION_OUTCOME_UNKNOWN,CRML_UI_ACTION_OUTCOME_UNKNOWN,
                          CRML_UI_ACTION_OUTCOME_UNKNOWN,CRML_UI_ACTION_EXPIRED,CRML_UI_ACTION_EXPIRED};
    if (crml_ui_action_status(receipt) != expected[step]) __builtin_trap();
    ++step;
    if (step == 1 || step == 3) submit();
    if (step == 5) {
        submit();crml_release();
        if (crml_ui_action_status(receipt) != CRML_UI_ACTION_CANCELLED) __builtin_trap();
    }
}
''', 'ui.read,ui.activate')
        # The second command is delivered without acknowledgement, then a new
        # page makes that uncertainty permanent. The third never leaves native.
        result=self.simulate({'schema':1,'capabilities':['ui.read','ui.activate'],
            'initial':{'ui':{'screen':1,'actions':1},'ui_outcome':'none'},'frames':[
                {'dt_ms':2001},
                {'dt_ms':10,'state':{'ui_new_page':True}},
                {'dt_ms':10,'state':{'ui_renderer':False}},
                {'dt_ms':2001},
                {'dt_ms':10,'state':{'ui_renderer':True}}]})
        self.assertEqual(result['failures'],0)

    def test_action_rebind_and_context_through_real_guest(self):
        self.c_guest('''
static unsigned tick;
void crml_init(void) {
    if (crml_input_bind(0, "W", 1) != 1) __builtin_trap();
}
void crml_tick(float dt) {
    (void)dt;
    crml_input_state input;
    if (crml_input_read(&input, sizeof(input)) != 1 || input.size != sizeof(input) || input.version != 1)
        __builtin_trap();
    const uint32_t flags[] = {15,15,11,7,31,15};
    const uint32_t held[] = {1,1,0,0,0,1};
    if (input.flags != flags[tick] || input.held != held[tick] ||
        input.binding_revision != (tick ? 3u : 2u) || input.bound != 1)
        __builtin_trap();
    if (crml_input_actions() != held[tick]) __builtin_trap();
    if (!tick && crml_input_bind(0, "N", 1) != 1) __builtin_trap();
    ++tick;
}
''','input.actions')
        result=self.simulate({'schema':1,'initial':{'keys':['W']},'frames':[
            {'dt_ms':10}, {'dt_ms':10,'state':{'keys':['N']}},
            {'dt_ms':10,'state':{'focused':False}},
            {'dt_ms':10,'state':{'focused':True,'input_fresh':False}},
            {'dt_ms':10,'state':{'input_fresh':True,'input_emergency':True}},
            {'dt_ms':10,'state':{'input_emergency':False}}]})
        self.assertEqual(result['failures'],0)

    def test_action_rebinding_does_not_change_another_mod(self):
        self.guest('''
          (import "crml_v1" "input_bind" (func $bind (param i32 i32 i32) (result i32)))
          (import "crml_v1" "input_actions" (func $actions (result i32)))
          (memory (export "memory") 1) (data (i32.const 0) "E")
          (func (export "crml_init") i32.const 0 i32.const 0 i32.const 1 call $bind drop)
          (func (export "crml_tick") (param f32) call $actions i32.const 1 i32.ne if unreachable end)
          ''','input.actions',name='a',bindings='action.0=W\n')
        self.guest('''
          (import "crml_v1" "input_actions" (func $actions (result i32)))
          (func (export "crml_init"))
          (func (export "crml_tick") (param f32) call $actions if unreachable end)
          ''','input.actions',name='b',bindings='action.0=W\n')
        self.assertEqual(self.simulate({'schema':1,'initial':{'keys':['E']},'frames':[{'dt_ms':10}]})['failures'],0)

    def test_input_emergency_cancels_simulated_leases_without_rearming(self):
        self.c_guest('''
void crml_init(void) {
    if (crml_motion_set(1, 0, 0, 0) != 1 || crml_visibility_set(1) != 1 ||
        crml_physics_select() != 0) __builtin_trap();
}
''','player.motion,player.visibility,physics.damping')
        self.simulate({'schema':1,'frames':[
            {'dt_ms':16,'state':{'input_emergency':True}},
            {'dt_ms':16,'state':{'input_emergency':False}}],
            'expect':[{'op':'motion_set'}, {'op':'visibility_set'}, {'op':'physics_select'},
                      {'op':'motion_cancel','frame':1}, {'op':'visibility_cancel','frame':1},
                      {'op':'physics_cancel','frame':1}]})

    def test_composed_observations_do_not_spend_mutation_budget(self):
        # A mod inspecting a prop while updating player presentation needs seven
        # reads and four commands. The former shared eight-call limit trapped
        # before the final commands despite every individual service being bounded.
        self.c_guest('''
void crml_init(void) {}
void crml_tick(float dt) {
    (void)dt;
    crml_player_state player;
    crml_camera_state camera;
    crml_physics_state physics;
    if (!crml_capabilities() || crml_input_actions()) __builtin_trap();
    crml_player_read(&player, sizeof(player));
    crml_camera_read(&camera, sizeof(camera));
    if (crml_physics_select_near(0, 0, 0, 2) != 0) __builtin_trap();
    if (crml_physics_status() != 3) __builtin_trap();
    uint64_t target = crml_physics_target();
    if (!target) __builtin_trap();
    crml_physics_read(target, &physics, sizeof(physics));
    if (crml_physics_apply(target, 2, 1000) != 0) __builtin_trap();
    if (crml_motion_set(1, 0, 0, 0) != 1) __builtin_trap();
    if (crml_visibility_set(1) != 1) __builtin_trap();
}
void crml_shutdown(void) { crml_release(); }
''', 'input.actions,player.read,camera.read,physics.damping,player.motion,player.visibility')
        result = self.simulate({'schema':1, 'initial':{'physics_status':3},
                               'frames':[{'dt_ms':100},{'dt_ms':100}]})
        self.assertEqual(result['failures'], 0)

    def test_full_read_and_command_allowances_reset_each_invocation(self):
        self.c_guest('''
static void exercise(void) {
    for (unsigned i = 0; i < 8; ++i)
        if (!(crml_capabilities() & CRML_CAP_PLAYER_VISIBILITY)) __builtin_trap();
    for (unsigned i = 0; i < 7; ++i)
        if (crml_visibility_set(1) != 1) __builtin_trap();
    crml_release();
}
void crml_init(void) { exercise(); }
void crml_tick(float dt) { (void)dt; exercise(); }
void crml_shutdown(void) { exercise(); }
''', 'player.visibility')
        result = self.simulate({'schema':1, 'frames':[{'dt_ms':100},{'dt_ms':100}]})
        self.assertEqual(result['failures'], 0)

    def test_feedback_delivery_and_expiry_through_wasm(self):
        self.c_guest('''
static int64_t receipt;
static unsigned step;
void crml_init(void) {
    if (!(crml_capabilities() & CRML_CAP_FEEDBACK)) __builtin_trap();
    receipt = crml_feedback_show("Hello", 5, CRML_FEEDBACK_INFO, 1000);
    if (receipt <= 0 || crml_feedback_status(receipt) != CRML_FEEDBACK_QUEUED) __builtin_trap();
    if (crml_feedback_show("Again", 5, 0, 1000) != -2) __builtin_trap();
}
void crml_tick(float dt) {
    (void)dt;
    const int expected[] = {CRML_FEEDBACK_QUEUED, CRML_FEEDBACK_PRESENTED, CRML_FEEDBACK_EXPIRED_PRESENTED};
    if (crml_feedback_status(receipt) != expected[step++]) __builtin_trap();
}
''', 'feedback')
        self.assertEqual(self.simulate({'schema':1, 'capabilities':['feedback'],
            'frames':[{'dt_ms':100},{'dt_ms':100},{'dt_ms':900}]} )['failures'], 0)

    def test_feedback_missing_renderer_and_rate_limit(self):
        self.c_guest('''
static int64_t receipt;
void crml_init(void) {
    receipt = crml_feedback_show("Hello", 5, 0, 1000);
    if (receipt <= 0 || crml_feedback_dismiss(receipt) != 1) __builtin_trap();
    if (crml_feedback_show("Rate", 4, 0, 1000) != -4) __builtin_trap();
}
void crml_tick(float dt) {
    (void)dt;
    if (crml_feedback_status(receipt) == CRML_FEEDBACK_DISMISSED) {
        receipt = crml_feedback_show("Retry", 5, 0, 1000);
        if (receipt <= 0) __builtin_trap();
    } else if (crml_feedback_status(receipt) != CRML_FEEDBACK_EXPIRED_UNPRESENTED) __builtin_trap();
}
''', 'feedback')
        self.assertEqual(self.simulate({'schema':1,'capabilities':['feedback'],
            'initial':{'feedback_renderer':False},'frames':[{'dt_ms':1000},{'dt_ms':1000}]})['failures'], 0)

    def test_shared_release_cancels_feedback(self):
        self.c_guest('''
void crml_init(void) {
    int64_t receipt = crml_feedback_show("Hello", 5, 0, 1000);
    if (receipt <= 0) __builtin_trap();
    crml_release();
    if (crml_feedback_status(receipt) != CRML_FEEDBACK_CANCELLED) __builtin_trap();
}
''', 'feedback')
        self.assertEqual(self.simulate({'schema':1,'capabilities':['feedback'],'frames':[]})['failures'], 0)

    def test_feedback_fault_releases_global_slot(self):
        imports = '''(import "crml_v1" "feedback_show" (func $show (param i32 i32 i32 i32) (result i64)))
          (memory (export "memory") 1) (data (i32.const 0) "hello")'''
        publish = 'i32.const 0 i32.const 5 i32.const 0 i32.const 1000 call $show i64.const 0 i64.le_s if unreachable end'
        self.guest(imports+'(func (export "crml_init") '+publish+' unreachable)', 'feedback', 'a-fault')
        for i in range(4):
            self.guest(imports+'(func (export "crml_init") '+publish+')', 'feedback', 'healthy-'+str(i))
        self.assertEqual(self.simulate({'schema':1,'capabilities':['feedback'],'frames':[], 'expect_failures':1})['failures'], 1)

    def test_visibility_observations_are_lease_local_and_do_not_renew(self):
        self.c_guest('''
static unsigned step;
static void check(unsigned status,unsigned flags,unsigned remaining) {
    crml_visibility_state s;
    if(crml_visibility_read(&s,sizeof(s))!=1 || s.version!=1 || s.state!=status ||
       s.flags!=flags || s.remaining_ms!=remaining) __builtin_trap();
}
void crml_init(void) {
    check(CRML_VISIBILITY_IDLE,0,0);
    if(crml_visibility_set(1)!=1) __builtin_trap();
    check(CRML_VISIBILITY_ACTIVE,0,500);
}
void crml_tick(float dt) {
    (void)dt;
    if(step==0) check(CRML_VISIBILITY_ACTIVE,0,499);
    if(step==1) check(CRML_VISIBILITY_ACTIVE,CRML_VISIBILITY_OBSERVED_HIDDEN,498);
    if(step==2) check(CRML_VISIBILITY_ACTIVE,CRML_VISIBILITY_OBSERVED_HIDDEN|CRML_VISIBILITY_SUBMITTED,1);
    if(step==3) {
        check(CRML_VISIBILITY_EXPIRED,CRML_VISIBILITY_OBSERVED_HIDDEN|CRML_VISIBILITY_SUBMITTED,0);
        if(crml_visibility_set(1)!=1) __builtin_trap();
        check(CRML_VISIBILITY_ACTIVE,0,500);
        crml_release();check(CRML_VISIBILITY_IDLE,0,0);
    }
    ++step;
}
''','player.visibility')
        self.guest('''(import "crml_v1" "visibility_read" (func $read (param i32 i32) (result i32)))
          (memory (export "memory") 1)
          (func $check
            i32.const 0 i32.const 16 call $read i32.const 1 i32.ne if unreachable end
            i32.const 0 i64.load i64.const 1 i64.ne if unreachable end
            i32.const 8 i64.load i64.eqz i32.eqz if unreachable end)
          (func (export "crml_init") call $check)
          (func (export "crml_tick") (param f32) call $check)''','player.visibility','z-observer')
        self.simulate({'schema':1,'frames':[{'dt_ms':1},
            {'dt_ms':1,'state':{'visibility_observation':'hidden'}},
            {'dt_ms':497,'state':{'visibility_observation':'submitted'}}, {'dt_ms':1}],
            'expect':[{'op':'visibility_set','owner':1,'result':1},
                      {'op':'visibility_cancel','owner':1,'time_ms':500},
                      {'op':'visibility_set','owner':1,'result':1}, {'op':'release','owner':1}]})

    def test_photo_visibility_requires_new_activation_after_lease_expiry(self):
        mod.build(ROOT/'examples/photo-visibility',self.mods/'photo-visibility')
        for mode in (0,1):
            with self.subTest(mode=mode):
                result=self.simulate({'schema':1,'initial':{'player':{'generation':1},
                    'settings':[{'mod':'photo-visibility','key':'mode','value':mode}]},
                    'frames':[{}, {'state':{'keys':['F8']}},
                              {'dt_ms':500,'state':{'visibility_observation':'submitted'}}, {},
                              {'state':{'keys':[]}}, {'state':{'keys':['F8']}},
                              {'state':{'visibility_observation':'hidden'}}],
                    'expect':[{'op':'visibility_set','frame':2,'result':1},
                              {'op':'visibility_cancel','frame':3},
                              {'op':'visibility_set','frame':3,'result':0},
                              {'op':'visibility_set','frame':6,'result':1},
                              {'op':'visibility_set','frame':7,'result':1},
                              {'op':'visibility_set','frame':7,'result':0}]})
                self.assertTrue(any('Photo visibility stopped.' in line for line in result['logs']))
                self.assertTrue(any('Player mesh hidden.' in line for line in result['logs']))

    def test_contention_failure_and_cleanup(self):
        body = '''(import "crml_v1" "visibility_set" (func $set (param i32) (result i32)))
          (func (export "crml_init")) (func (export "crml_tick") (param f32) i32.const 1 call $set drop)'''
        self.guest(body, 'player.visibility', 'first')
        self.guest(body, 'player.visibility', 'second')
        result = self.simulate({'schema': 1, 'frames': [{}, {'state': {'returns': {'visibility_set': -1}}}],
           'expect': [
               {'op': 'visibility_set', 'owner': 1, 'result': 1},
               {'op': 'visibility_set', 'owner': 2, 'result': -2},
               {'op': 'visibility_set', 'owner': 1, 'result': -1},
               {'op': 'visibility_set', 'owner': 2, 'result': -2},
               {'op': 'release', 'owner': 1}]})
        self.assertEqual(result['failures'], 0)

    def test_trap_releases_owner_before_other_mod(self):
        imports = '(import "crml_v1" "visibility_set" (func $set (param i32) (result i32)))'
        self.guest(imports+'''(func (export "crml_init") i32.const 1 call $set drop)
                             (func (export "crml_tick") (param f32) unreachable)''', 'player.visibility', 'a-trap')
        self.guest(imports+'''(func (export "crml_init"))
                             (func (export "crml_tick") (param f32) i32.const 1 call $set drop)''', 'player.visibility', 'b-healthy')
        result = self.simulate({'schema': 1, 'frames': [{}], 'expect_failures': 1,
           'expect': [{'op': 'visibility_set', 'owner': 1}, {'op': 'release', 'owner': 1},
                      {'op': 'visibility_set', 'owner': 2, 'result': 1}, {'op': 'release', 'owner': 2}]})
        self.assertTrue(any('Disabled a-trap' in line for line in result['logs']))

    def test_motion_receipt_survives_failed_renewal_and_explicit_cleanup(self):
        self.c_guest('''
static unsigned step;
static void check(unsigned status, unsigned reason, unsigned flags) {
    crml_motion_state s;
    if(crml_motion_read(&s,sizeof(s))!=1 || s.version!=1 ||
       s.state!=status || s.stop_reason!=reason || s.flags!=flags) __builtin_trap();
}
void crml_init(void) {
    check(CRML_MOTION_IDLE,0,0);
    if(crml_motion_set(1,0,0,0)!=1) __builtin_trap();
    check(CRML_MOTION_ACTIVE,0,0);
}
void crml_tick(float dt) {
    (void)dt;
    if(step==0) {
        check(CRML_MOTION_STOPPED,CRML_MOTION_STOP_LEASE,CRML_MOTION_CANCEL_PENDING);
        check(CRML_MOTION_STOPPED,CRML_MOTION_STOP_LEASE,CRML_MOTION_CANCEL_PENDING);
        if(crml_motion_set(1,0,0,0)!=-1) __builtin_trap();
        check(CRML_MOTION_STOPPED,CRML_MOTION_STOP_LEASE,0);
    } else if(step==1) {
        check(CRML_MOTION_STOPPED,CRML_MOTION_STOP_LEASE,0);
        if(crml_motion_set(1,0,0,0)!=1) __builtin_trap();
        check(CRML_MOTION_ACTIVE,0,0);
        crml_release();
        check(CRML_MOTION_IDLE,0,0);
    } else {
        if(crml_motion_set(1,0,0,0)!=1 || crml_motion_set(0,0,0,0)!=0) __builtin_trap();
        check(CRML_MOTION_IDLE,0,0);
    }
    ++step;
}
''','player.motion')
        self.simulate({'schema':1,'frames':[{'dt_ms':501},{'dt_ms':1},{'dt_ms':1}],
            'expect':[{'op':'motion_set','result':1}, {'op':'motion_cancel'},
                      {'op':'motion_set','result':-1}, {'op':'motion_set','result':1},
                      {'op':'release'}, {'op':'motion_set','result':1}, {'op':'motion_set','result':0}]})

    def test_motion_observation_does_not_extend_lease(self):
        self.c_guest('''
static unsigned step;
void crml_init(void) {if(crml_motion_set(1,0,0,0)!=1) __builtin_trap();}
void crml_tick(float dt) {
    (void)dt;
    for(unsigned i=0;i<8;++i) {
        crml_motion_state s;
        if(crml_motion_read(&s,sizeof(s))!=1 || s.version!=1) __builtin_trap();
        if(!step) {
            if(s.state!=CRML_MOTION_ACTIVE || s.stop_reason || s.flags) __builtin_trap();
        } else if(s.state!=CRML_MOTION_STOPPED || s.stop_reason!=CRML_MOTION_STOP_LEASE ||
                  s.flags!=CRML_MOTION_CANCEL_PENDING) __builtin_trap();
    }
    ++step;
}
''','player.motion')
        self.simulate({'schema':1,'frames':[{'dt_ms':499},{'dt_ms':2}],
            'expect':[{'op':'motion_set','time_ms':0}, {'op':'motion_cancel','time_ms':501}]})

    def test_lease_expiry_before_tick(self):
        self.guest('''(import "crml_v1" "motion_set" (func $set (param i32 f32 f32 f32) (result i32)))
          (func (export "crml_init") i32.const 1 f32.const 1 f32.const 0 f32.const 0 call $set drop)''', 'player.motion')
        self.simulate({'schema': 1, 'frames': [{'dt_ms': 500}, {'dt_ms': 1}],
                      'expect': [{'op': 'motion_set', 'time_ms': 0}, {'op': 'motion_cancel', 'time_ms': 501}]})

    def test_named_input_is_manifest_scoped_and_focus_gated(self):
        self.guest('''(import "crml_v1" "input_actions" (func $input (result i32)))
          (global $step (mut i32) (i32.const 0)) (func (export "crml_init"))
          (func (export "crml_tick") (param f32)
            global.get $step i32.eqz if call $input i32.const 1 i32.ne if unreachable end
            else call $input if unreachable end end
            global.get $step i32.const 1 i32.add global.set $step)''', 'input.actions', bindings='action.0=Home\naction.1=End\n')
        self.simulate({'schema': 1, 'frames': [{'state': {'keys': ['Home', 'F8']}},
                                            {'state': {'focused': False}},
                                            {'state': {'focused': True, 'input_fresh': False}}], 'expect': []})

    def test_expired_movement_reports_cancellation_before_rearm(self):
        mod.build(ROOT/'examples/movement', self.mods/'movement')
        self.simulate({'schema': 1, 'initial': {'heading': {'right': [1, 0]}}, 'frames': [
            {'dt_ms': 10, 'state': {'keys': ['F6', 'W']}},
            {'dt_ms': 500, 'state': {'keys': ['W']}}, {'dt_ms': 501},
            {'dt_ms': 10, 'state': {'keys': []}}],
            'expect': [{'op': 'motion_set', 'frame': 1, 'result': 1},
                       {'op': 'motion_set', 'frame': 2, 'result': 1},
                       {'op': 'motion_cancel', 'frame': 3},
                       {'op': 'motion_set', 'frame': 3, 'result': -1},
                       {'op': 'motion_set', 'frame': 4, 'result': 0},
                       {'op': 'motion_set', 'frame': 4, 'result': 0}]})

    def test_movement_cancellation_survives_another_owner_cycle(self):
        imports = '''(import "crml_v1" "motion_set" (func $motion (param i32 f32 f32 f32) (result i32)))
          (import "crml_v1" "motion_read" (func $read (param i32 i32) (result i32)))
          (memory (export "memory") 1)
          (func $check (param $state i32) (param $reason i32) (param $flags i32)
            i32.const 0 i32.const 16 call $read i32.const 1 i32.ne if unreachable end
            i32.const 0 i32.load i32.const 1 i32.ne if unreachable end
            i32.const 4 i32.load local.get $state i32.ne if unreachable end
            i32.const 8 i32.load local.get $reason i32.ne if unreachable end
            i32.const 12 i32.load local.get $flags i32.ne if unreachable end)
          (global $step (mut i32) (i32.const 0))'''
        self.guest(imports+'''(func (export "crml_init")
            i32.const 1 f32.const 0 f32.const 0 f32.const 0 call $motion drop)
          (func (export "crml_tick") (param f32)
            global.get $step i32.const 1 i32.eq if
              i32.const 2 i32.const 4 i32.const 1 call $check
              i32.const 1 f32.const 0 f32.const 0 f32.const 0 call $motion
              i32.const -1 i32.ne if unreachable end
              i32.const 2 i32.const 4 i32.const 0 call $check
            end
            global.get $step i32.const 1 i32.add global.set $step)''', 'player.motion', 'a-cancelled')
        self.guest(imports+'''(func (export "crml_init"))
          (func (export "crml_tick") (param f32)
            global.get $step i32.eqz if
              i32.const 0 i32.const 0 i32.const 0 call $check
              i32.const 1 f32.const 0 f32.const 0 f32.const 0 call $motion
              i32.const 1 i32.ne if unreachable end
              i32.const 1 i32.const 0 i32.const 0 call $check
              i32.const 0 f32.const 0 f32.const 0 f32.const 0 call $motion
              if unreachable end
              i32.const 0 i32.const 0 i32.const 0 call $check
            end
            global.get $step i32.const 1 i32.add global.set $step)''', 'player.motion', 'b-temporary')
        self.simulate({'schema': 1, 'frames': [{'dt_ms': 501}, {'dt_ms': 10}],
            'expect': [{'op': 'motion_set', 'owner': 1, 'frame': 0, 'result': 1},
                       {'op': 'motion_cancel', 'owner': 1},
                       {'op': 'motion_set', 'owner': 2, 'result': 1},
                       {'op': 'motion_set', 'owner': 2, 'result': 0},
                       {'op': 'motion_set', 'owner': 1, 'result': -1}]})

    def test_explicit_release_acknowledges_movement_cancellation(self):
        self.c_guest('''
void crml_init(void) {if(crml_motion_set(1,0,0,0)!=1) __builtin_trap();}
void crml_tick(float dt) {
    (void)dt;
    crml_release();
    if(crml_motion_set(1,0,0,0)!=1) __builtin_trap();
}
''', 'player.motion')
        # First release an active lease, then release after automatic expiry.
        self.simulate({'schema': 1, 'frames': [{'dt_ms': 10}, {'dt_ms': 501}],
            'expect': [{'op': 'motion_set', 'owner': 1, 'frame': 0, 'result': 1},
                       {'op': 'release', 'owner': 1, 'frame': 1},
                       {'op': 'motion_set', 'owner': 1, 'frame': 1, 'result': 1},
                       {'op': 'motion_cancel', 'owner': 1, 'frame': 2},
                       {'op': 'motion_set', 'owner': 1, 'frame': 2, 'result': 1},
                       {'op': 'release', 'owner': 1, 'frame': 2}]})

    def test_release_cannot_claim_success_for_foreign_owner(self):
        imports = '''(import "crml_v1" "visibility_set" (func $visibility (param i32) (result i32)))
                     (import "crml_v1" "motion_set" (func $motion (param i32 f32 f32 f32) (result i32)))'''
        self.guest(imports+'''(func (export "crml_init") i32.const 1 call $visibility drop
                     i32.const 1 f32.const 0 f32.const 0 f32.const 0 call $motion drop)''',
                   'player.motion,player.visibility', 'a-owner')
        self.guest(imports+'''(func (export "crml_init")) (func (export "crml_tick") (param f32)
                     i32.const 0 call $visibility i32.const -2 i32.ne if unreachable end
                     i32.const 0 f32.const 0 f32.const 0 f32.const 0 call $motion i32.const -2 i32.ne if unreachable end)''',
                   'player.motion,player.visibility', 'b-other')
        self.simulate({'schema': 1, 'frames': [{}], 'expect': [
            {'op': 'visibility_set', 'owner': 1, 'result': 1}, {'op': 'motion_set', 'owner': 1, 'result': 1},
            {'op': 'visibility_set', 'owner': 2, 'result': -2}, {'op': 'motion_set', 'owner': 2, 'result': -2},
            {'op': 'release', 'owner': 1}]})

    def test_unavailable_release_is_unavailable(self):
        self.c_guest('''void crml_init(void) {
            if(crml_motion_set(0,0,0,0)!=-1 || crml_visibility_set(0)!=-1 || crml_physics_restore()!=-1) __builtin_trap();
        }''', 'player.motion,player.visibility,physics.damping')
        self.simulate({'schema': 1, 'capabilities': [], 'frames': [], 'expect': [
            {'op': 'motion_set', 'result': -1}, {'op': 'visibility_set', 'result': -1},
            {'op': 'physics_restore', 'result': -1}]})

    def test_snapshot_focus_and_keyboard_semantics(self):
        self.c_guest('''static unsigned step;
        void crml_init(void) {}
        void crml_tick(float dt) {
            (void)dt;crml_player_state player;crml_camera_state camera;
            if(crml_camera_read(&camera,sizeof(camera))!=1) __builtin_trap();
            if(crml_player_read(&player,sizeof(player))!=(step?1:0)) __builtin_trap();
            ++step;
        }''', 'player.read,camera.read')
        self.simulate({'schema': 1, 'initial': {'player': {}, 'camera': {}}, 'frames': [
            {'state': {'focused': False}}, {'state': {'focused': True, 'input_fresh': False}}], 'expect': []})

    def test_navigation_snapshot_scripted_up_sequence_and_unavailable_zeroing(self):
        zeros=' '.join(f'i32.const {offset} i64.load i64.eqz i32.eqz if unreachable end' for offset in range(0,56,8))
        self.guest(f"""(import "crml_v1" "navigation_read" (func $read (param i32 i32) (result i32)))
          (memory (export "memory") 1) (global $step (mut i32) (i32.const 0))
          (func (export "crml_init")) (func (export "crml_tick") (param f32)
            global.get $step i32.const 2 i32.lt_u if
              i32.const 0 i32.const 56 call $read i32.const 1 i32.ne if unreachable end
              global.get $step i32.eqz if
                i32.const 8 i64.load i64.const 0xf123456789abcdef i64.ne if unreachable end
                i32.const 16 i64.load i64.const 0xe123456789abcdef i64.ne if unreachable end
                i32.const 24 i32.load i32.const 3 i32.ne if unreachable end
                i32.const 32 f32.load f32.const 3.5 f32.ne if unreachable end
                i32.const 44 f32.load f32.const -1 f32.ne if unreachable end
              else
                i32.const 24 i32.load if unreachable end
                i32.const 44 i64.load i64.eqz i32.eqz if unreachable end
                i32.const 52 i32.load if unreachable end
              end
            else
              i32.const 0 i32.const 255 i32.const 56 memory.fill
              i32.const 0 i32.const 56 call $read
              global.get $step i32.const 5 i32.eq if (result i32) i32.const -1 else i32.const 0 end
              i32.ne if unreachable end {zeros}
            end
            global.get $step i32.const 1 i32.add global.set $step)""", 'navigation.read')
        self.simulate({'schema':1,'capabilities':['navigation.read'],'frames':[
            {'state':{'navigation':{'generation':0xf123456789abcdef,'sequence':0xe123456789abcdef,
                                   'position':[3.5,0,1],'up':[-1,0,0],'flags':3}}},
            {'state':{'navigation':{'up':[0,1,0]}}},
            {'state':{'focused':False}},
            {'state':{'focused':True,'input_emergency':True}},
            {'state':{'input_emergency':False,'navigation':{'age_ms':501}}},
            {'state':{'navigation':{'result':-1}}}], 'expect':[]})

    def test_navigation_simulation_rejects_malformed_direction_or_identity(self):
        for value in [{'flags':1},{'flags':1,'up':[0,2,0]},{'flags':16},{'generation':0},
                      {'sequence':0},{'sequence':2**64},{'up':[0,1]},{'position':[float('nan'),0,0]}]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                mod.scenario_protocol({'schema':1,'initial':{'navigation':value},'frames':[]})

    def test_player_snapshot_full_generation_and_failure_zeroing(self):
        self.guest('''(import "crml_v1" "player_read" (func $read (param i32 i32) (result i32)))
          (memory (export "memory") 1) (global $step (mut i32) (i32.const 0))
          (func (export "crml_init")) (func (export "crml_tick") (param f32)
            global.get $step i32.eqz if
              i32.const 0 i32.const 32 call $read i32.const 1 i32.ne if unreachable end
              i32.const 8 i64.load i64.const 0xf123456789abcdef i64.ne if unreachable end
              i32.const 16 f32.load f32.const 3.5 f32.ne if unreachable end
            else
              i32.const 0 i32.const 32 call $read if unreachable end
              i32.const 0 i64.load i32.const 8 i64.load i64.or
              i32.const 16 i64.load i64.or i32.const 24 i64.load i64.or i64.eqz i32.eqz if unreachable end
            end i32.const 1 global.set $step)''', 'player.read')
        self.simulate({'schema': 1, 'frames': [
          {'state': {'player': {'generation': 0xf123456789abcdef, 'position': [3.5, 0, 1]}}},
          {'state': {'player': {'result': 0}}}], 'expect': []})

    def test_wrong_expectation_fails(self):
        self.guest('(func (export "crml_init"))', '')
        with self.assertRaisesRegex(ValueError, 'Expected 1 events'):
            self.simulate({'schema': 1, 'frames': [], 'expect': [{'op': 'motion_set'}]})

    def test_empty_mods_cannot_pass_a_scenario(self):
        with self.assertRaisesRegex(ValueError,'No mod packages loaded'):
            self.simulate({'schema':1,'frames':[{}],'expect':[]})

    def test_ui_presentation_ownership_uses_native_service(self):
        self.c_guest('''
void crml_init(void) {
    crml_ui_state s;
    if(crml_ui_read(&s,sizeof(s))!=1) __builtin_trap();
    if(crml_ui_present(s.generation,CRML_UI_TARGET_CLASS,"splash",6,1,750)!=0) __builtin_trap();
    if(crml_ui_present(s.generation,CRML_UI_TARGET_CLASS,"splash",6,0,0)!=0) __builtin_trap();
}
''','ui.read,ui.presentation')
        self.simulate({'schema':1,'initial':{'ui':{'screen':1,'actions':1}},'frames':[],
            'expect':[{'op':'ui_present','result':0,'args':[1,2,1,750],'target':'splash'},
                      {'op':'ui_present','result':0,'args':[1,2,0,0],'target':'splash'}]})
        with self.assertRaisesRegex(ValueError,'differs'):
            self.simulate({'schema':1,'initial':{'ui':{'screen':1,'actions':1}},'frames':[],
                'expect':[{'op':'ui_present','target':'hud'}, {'op':'ui_present'}]})

    def test_physics_queue_snapshot_and_token_retirement(self):
        self.c_guest('''
static unsigned step; static uint64_t token;
void crml_init(void) {if(crml_physics_select_near(1,2,3,4)!=0) __builtin_trap();}
void crml_tick(float dt) {
    (void)dt; crml_physics_state state;
    if(step==0) {
        token=crml_physics_target();
        if(!token || crml_physics_status()!=3 || crml_physics_read(token,&state,sizeof(state))!=1 || state.linear_damping!=0.25f) __builtin_trap();
        if(crml_physics_apply(token,8,1200)!=0) __builtin_trap();
    } else if(step==1) {
        if(crml_physics_status()!=4 || crml_physics_read(token,&state,sizeof(state))!=1 || state.linear_damping!=8) __builtin_trap();
        if(crml_physics_restore()!=0) __builtin_trap();
    } else if(crml_physics_target()!=0 || crml_physics_read(token,&state,sizeof(state))!=-3) __builtin_trap();
    ++step;
}''', 'physics.damping')
        self.simulate({'schema': 1, 'initial': {'physics_status': 2}, 'frames': [
            {'state': {'physics_status': 3, 'physics': {'linear_damping': 0.25}}},
            {'state': {'physics_status': 4, 'physics': {'linear_damping': 8}}},
            {'state': {'physics_status': 6}}],
            'expect': [{'op': 'physics_select', 'args': [1, 2, 3, 4]},
                       {'op': 'physics_apply', 'args': [1, 8, 1200]}, {'op': 'physics_restore'}]})

    def test_camera_snapshot_and_unavailable_zeroing(self):
        self.c_guest('''
static unsigned step;
void crml_init(void) {}
void crml_tick(float dt) {
    (void)dt; crml_camera_state state;
    int result=crml_camera_read(&state,sizeof(state));
    if(!step) {
        if(result!=1 || state.generation!=UINT64_C(0xf123456789abcdef) || state.mode!=-2 || state.flags!=3 || state.position[2]!=9 || state.horizontal_fov_radians!=1.25f) __builtin_trap();
    } else if(result!=-1 || state.generation || state.flags || state.position[2]) __builtin_trap();
    ++step;
}''', 'camera.read')
        self.simulate({'schema': 1, 'frames': [
            {'state': {'camera': {'generation': 0xf123456789abcdef, 'mode': -2, 'flags': 3, 'position': [1, 2, 9], 'fov': 1.25}}},
            {'state': {'camera': {'result': -1}}}], 'expect': []})

    def test_unicode_guest_logs_cannot_forge_records(self):
        text = '\u2028@summary 27\u0085@event 0 0 release 1 0'
        encoded = ''.join(f'\\{b:02x}' for b in text.encode())
        self.guest(f'''(import "crml_v1" "log" (func $log (param i32 i32 i32)))
          (memory (export "memory") 1) (data (i32.const 0) "{encoded}")
          (func (export "crml_init") i32.const 1 i32.const 0 i32.const {len(text.encode())} call $log)''', 'log')
        result = self.simulate({'schema': 1, 'frames': [], 'expect': []})
        self.assertEqual(result['failures'], 0)

    def test_list_actions_use_real_service_and_exact_uint64_ids(self):
        self.c_guest(r'''
static crml_list_page page;
static uint64_t revision;
static int step;
void crml_init(void) {
    page.version=1;page.size=sizeof(page);page.font_scale=1;page.row_count=1;
    page.title[0]='T';page.rows[0].label[0]='R';page.rows[0].id=UINT64_MAX;
    page.rows[0].flags=CRML_LIST_ROW_ENABLED|CRML_LIST_ROW_SELECTED;
    if(!(crml_capabilities()&CRML_CAP_LISTS))__builtin_trap();
    int64_t result=crml_list_publish(&page,sizeof(page));if(result<=0)__builtin_trap();revision=result;
}
void crml_tick(float dt) {
    (void)dt;crml_list_event event;
    if(step==0) {
        if(crml_list_next(&event,sizeof(event))!=1 || event.row_id!=UINT64_MAX || event.revision!=revision || event.sequence!=1)__builtin_trap();
        if(crml_list_next(&event,sizeof(event))!=0 || event.row_id || event.version)__builtin_trap();
        page.rows[0].label[0]='S';int64_t next=crml_list_publish(&page,sizeof(page));if(next<=(int64_t)revision)__builtin_trap();revision=next;
    } else if(step==1) {
        if(crml_list_next(&event,sizeof(event))!=1 || event.row_id!=UINT64_MAX || event.revision!=revision || event.sequence!=2)__builtin_trap();
        if(crml_list_next(&event,sizeof(event))!=0)__builtin_trap();
    } else if(step==2) {
        if(crml_capabilities()&CRML_CAP_LISTS)__builtin_trap();
        if(crml_list_next(&event,sizeof(event))!=-1 || event.version || event.row_id)__builtin_trap();
        if(crml_list_publish(&page,sizeof(page))!=-1)__builtin_trap();
    } else if(step==3) {
        if(!(crml_capabilities()&CRML_CAP_LISTS) || crml_list_next(&event,sizeof(event))!=0)__builtin_trap();
        if(crml_list_publish(&page,sizeof(page))<=(int64_t)revision)__builtin_trap();
    } else if(step==4) {if(crml_list_hide()!=1)__builtin_trap();}
    ++step;
}''', 'lists')
        row=2**64-1
        document={'schema':1,'capabilities':['lists'],
                  'initial':{'list_actions':[{'mod':'test','row':row}]},
                  'frames':[{'dt_ms':100},{'dt_ms':100,'state':{'list_actions':[
                      {'mod':'test','row':row,'revision':1},{'mod':'test','row':row}]}},
                      {'dt_ms':100,'state':{'lists_renderer':False}},
                      {'dt_ms':100,'state':{'lists_renderer':True}},{'dt_ms':100}],
                  'expect':[
                      {'op':'list_activate','frame':0,'result':200,'args':[1,row]},
                      {'op':'list_page','frame':0,'result':1,'args':[1,1,row]},
                      {'op':'list_page','frame':1,'result':1,'args':[2,1,row]},
                      {'op':'list_activate','frame':2,'result':409,'args':[1,row]},
                      {'op':'list_activate','frame':2,'result':200,'args':[2,row]},
                      {'op':'list_page','frame':3,'result':0,'args':[2,0,0]},
                      {'op':'list_page','frame':4,'result':1,'args':[3,1,row]},
                      {'op':'list_page','frame':5,'result':0,'args':[3,0,0]}]}
        self.simulate(document)
        document['expect'][0]['args'][1]=row-1
        with self.assertRaisesRegex(ValueError,'differs'):
            self.simulate(document)

    def test_list_queue_full_does_not_discard_accepted_events(self):
        self.c_guest(r'''
static crml_list_page page;static int step;
void crml_init(void){page.version=1;page.size=sizeof(page);page.font_scale=1;page.row_count=1;page.title[0]='T';
page.rows[0].id=7;page.rows[0].flags=1;page.rows[0].label[0]='R';if(crml_list_publish(&page,sizeof(page))<=0)__builtin_trap();}
void crml_tick(float dt){(void)dt;if(++step>1){for(int i=0;i<8;++i){crml_list_event event;
if(crml_list_next(&event,sizeof(event))!=1 || event.row_id!=7 || event.sequence!=(uint64_t)((step-2)*8+i+1))__builtin_trap();}}}
''','lists')
        result=self.simulate({'schema':1,'capabilities':['lists'],'initial':{'list_actions':[{'mod':'test','row':7}]*16},
                             'frames':[{}, {'state':{'list_actions':[{'mod':'test','row':7}]}}, {}]})
        actions=[event for event in result['events'] if event['op']=='list_activate']
        self.assertEqual([e['result'] for e in actions],[200]*16+[429])
        self.assertEqual(result['events'][-1]['result'],0)
        self.assertEqual(result['events'][-1]['op'],'list_page')

    def test_list_initial_renderer_unavailable(self):
        self.c_guest(r'''
void crml_init(void){crml_list_event event;
if(crml_capabilities()&CRML_CAP_LISTS)__builtin_trap();
if(crml_list_next(&event,sizeof(event))!=-1 || event.version)__builtin_trap();}
''','lists')
        self.simulate({'schema':1,'capabilities':['lists'],'initial':{'lists_renderer':False},'frames':[],'expect':[]})
        self.simulate({'schema':1,'capabilities':[],'frames':[],'expect':[]})

    def test_invalid_scenario_is_rejected(self):
        base = {'schema': 1, 'frames': []}
        cases = [dict(base, capabilities=['player.noclip']), dict(base, typo=True), dict(base, schema=True),
                 dict(base, expect=[{'op': []}]),
                 dict(base, initial={'keys': ['W\nstart']}), dict(base, initial={'focused': 1}),
                 dict(base, initial={'input_emergency': 1}),
                 dict(base, frames=[{'dt_ms': -1}]), dict(base, initial={'player': {'position': [1, 2]}}),
                 dict(base, initial={'heading': {'right': [float('nan'), 0]}}),
                 dict(base, initial={'heading': {'right': [10**1000, 0]}}),
                 dict(base, initial={'returns': {'motion_set': 1}}),
                 dict(base, initial={'settings':[{'mod':'test\nstart','key':'x','value':1}]}),
                 dict(base, initial={'media':{'name':'boot.tex\nfinish'}}),
                 dict(base, initial={'media':{'ready':1}}),
                 dict(base, initial={'media':{'source':[]}}),
                 dict(base, initial={'visibility_observation':3}),
                 dict(base, initial={'visibility_observation':'rendered'}),
                 dict(base, initial={'visibility_observation':[]}),
                 dict(base, initial={'lists_renderer':1}),
                 dict(base, initial={'list_actions':[{'mod':'test','row':1}]*17}),
                 dict(base, initial={'list_actions':[{'mod':'bad\nstart','row':1}]}),
                 dict(base, initial={'list_actions':[{'mod':'test','row':2**64}]}),
                 dict(base, initial={'list_actions':[{'mod':'test','row':1,'revision':0}]}),
                 dict(base, initial={'list_actions':[{'mod':'test','row':True}]}),
                 dict(base, initial={'list_actions':[{'mod':'test','row':1.0}]})]
        for case in cases:
            with self.subTest(case=str(case)[:100]), self.assertRaises(ValueError):
                mod.scenario_protocol(case)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    BIN = args.bin.resolve()
    unittest.main(argv=[__file__, *remaining])
