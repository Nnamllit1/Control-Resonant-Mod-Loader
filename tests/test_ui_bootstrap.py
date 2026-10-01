"""Exercise the authored UI bridge with a bounded fake transport in headless Chromium.

No game assets or network requests are used. This tests protocol and lifecycle
behavior; XHR routing through the embedded game renderer still needs a live check.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
from pathlib import Path
import subprocess
import time
import urllib.request

from test_native_ui_page import Browser, default_browser

ROOT = Path(__file__).resolve().parents[1]
NONCE = '18446744073709551615'
SHELL = """<!doctype html><html><head><meta charset="utf-8"></head><body><script>
(function(){
 var now=1000, callbacks=[], requests=[], events=[], fail=false, stopOnTrigger=false, throwTrigger=false;
 Date.now=function(){return now};
 window.setInterval=function(fn){callbacks.push(fn);return callbacks.length};
 window.clearInterval=function(id){callbacks[id-1]=null};
 function models(screen,loading,fading,splash){
  window.ui_splash_screen_current={value:screen};
  window.ui_splash_screen_sequence_loading={value:loading};
  window.ui_splash_screen_sequence_fading_out={value:fading};
  window.ui_stacks_program_flow_states_splash_screen_current={value:splash};
  window.ui_stacks_program_flow_state={value:'splash_screen'};
 }
 models(1,false,false,true);
 window.engine={trigger:function(name){events.push(name);if(stopOnTrigger)__crmlUiBridge.stop();if(throwTrigger)throw Error('fixture trigger failed')}};
 window.XMLHttpRequest=function(){this.status=0;this.responseText='';this.aborted=false;requests.push(this)};
 XMLHttpRequest.prototype.open=function(method,url,async){this.method=method;this.url=url;this.async=async;if(fail)throw Error('fixture open failed')};
 XMLHttpRequest.prototype.send=function(){this.sent=true};
 XMLHttpRequest.prototype.abort=function(){this.aborted=true;if(this.onabort)this.onabort()};
 window.fixture={models:models,requests:requests,events:events,
  tick:function(ms){now+=ms;callbacks.slice().forEach(function(fn){if(fn)fn()})},
  finish:function(index,value,status){var x=requests[index];x.status=status===undefined?200:status;x.responseText=typeof value==='string'?value:JSON.stringify(value);if(x.onload)x.onload()},
  fail:function(value){fail=value},stopOnTrigger:function(){stopOnTrigger=true},throwTrigger:function(){throwTrigger=true},
  timers:function(){return callbacks.filter(function(fn){return !!fn}).length}};
}());
</script><div id="fixture-panel" class="fixture-panel" style="visibility:visible"><span>Panel</span></div>
<div id="fixture-other" class="fixture-other" style="visibility:collapse">Other</div>PAYLOAD</body></html>"""


async def check(browser: Browser, fixture: Path):
    async def js(source):
        return await browser.evaluate(source)

    async def fresh():
        await browser.call('Page.navigate', url=fixture.as_uri())
        await browser.wait('!!window.__crmlUiBridge && fixture.requests.length===1')

    async def finish(value, index=-1, status=200):
        await js(f'fixture.finish({index if index >= 0 else "fixture.requests.length-1"},{json.dumps(value)},{status})')

    def command(identifier, screen=1, **extra):
        return dict(id=identifier, generation='18446744073709551615', action=1, screen=screen, **extra)

    await fresh()
    assert await js('fixture.requests[0].url') == f'coui://base/crml/ui/v1/{NONCE}/1/1/0/0'
    assert await js('fixture.requests[0].timeout===1500 && fixture.requests[0].method==="GET" && fixture.requests[0].async')
    await js('fixture.tick(1000)')
    assert await js('fixture.requests.length') == 1, 'only one request may be in flight'
    await finish({'id': 0})
    await js('fixture.tick(250)')
    assert await js('fixture.requests[1].url') == f'coui://base/crml/ui/v1/{NONCE}/2/1/1/0'
    await finish(command(1))
    assert await js('fixture.events') == ['splash_continue_pressed']
    await js('fixture.tick(250)')
    assert (await js('fixture.requests[2].url')).endswith('/1/0/1')
    await finish(command(1))
    assert await js('fixture.events.length') == 1, 'duplicate command must not execute'
    await js('fixture.tick(250)')
    await finish(dict(id=2, generation='18446744073709551616', action=1, screen=1))
    await js('fixture.tick(250)')
    assert (await js('fixture.requests[fixture.requests.length-1].url')).endswith('/1/0/2'), 'invalid command must still be acknowledged'
    await finish(command(1))
    assert await js('fixture.events.length') == 1, 'older ID cannot replay after rejection'
    await js('fixture.tick(250)')
    await finish(command(3))
    assert await js('fixture.events.length') == 1, 'new command ID cannot repeat Continue in same visit'
    await js('fixture.models(1,true,false,true);fixture.tick(250)')
    await finish({'id': 0})
    await js('fixture.models(1,false,false,true);fixture.tick(250)')
    await finish({'id': 0})
    await js('fixture.tick(1000)')
    await finish(command(4))
    assert await js('fixture.events.length') == 1, 'readiness changes do not rearm a dispatched screen'
    await js('fixture.models(5,false,false,true);fixture.tick(250)')
    await finish({'id': 0})
    await js('fixture.models(1,false,false,true);fixture.tick(250)')
    await finish({'id': 0})
    await js('fixture.tick(1000)')
    await finish(command(5))
    assert await js('fixture.events.length') == 2, 'a new visit rearms Continue'

    # Returning to the same screen during one pending request still invalidates it.
    for transition in ('fixture.models(5,false,false,true)', 'fixture.models(1,true,false,true)'):
        await fresh()
        await js('fixture.tick(1000);' + transition + ';fixture.tick(250);'
                 'fixture.models(1,false,false,true);fixture.tick(250);fixture.tick(750)')
        await finish(command(1))
        assert await js('fixture.events.length') == 0, 'observed screen/readiness epoch must match request'
        await js('fixture.tick(250)')
        assert (await js('fixture.requests[1].url')).endswith('/1/1/1')
        await finish(command(2))
        assert await js('fixture.events.length') == 1

    # Readiness changes invalidate responses; blocked categories never expose an action.
    for screen in (2, 3, 4, 6, 7):
        await fresh()
        await js(f'fixture.models({screen},false,false,true);fixture.tick(1000)')
        await finish(command(1, screen))
        await js('fixture.tick(1000)')
        assert (await js('fixture.requests[1].url')).endswith(f'/{screen}/0/1')
        assert await js('fixture.events.length') == 0
    for expression in ('fixture.models(1,true,false,true)', 'fixture.models(1,false,true,true)',
                       'fixture.models(1,false,false,false)', 'fixture.models(1,0,false,true)',
                       'delete window.ui_splash_screen_sequence_loading', 'window.engine.trigger=null'):
        await fresh()
        await js('fixture.tick(1000);' + expression)
        await finish(command(1))
        assert await js('fixture.events.length') == 0, expression

    await fresh()
    await js('fixture.tick(1000);fixture.models(5,false,false,true)')
    await finish(command(1, 1))
    await js('fixture.tick(250)')
    await finish(command(2, 5))
    assert await js('fixture.events.length') == 0, 'new screen must stabilize before action'
    await js('fixture.tick(750)')
    await finish(command(3, 5))
    assert await js('fixture.events.length') == 1
    await js('fixture.tick(250);fixture.models(8,false,false,true)')
    await finish({'id': 0})
    await js('fixture.tick(250)')
    await finish({'id': 0})
    await js('fixture.tick(750)')
    await finish(command(4, 8))
    assert await js('fixture.events.length') == 2

    # Timeout closes one sequence; its late callback cannot replace a newer reply.
    await fresh()
    await js('window.late=fixture.requests[0].onload;fixture.requests[0].ontimeout();fixture.tick(1000)')
    await js('fixture.requests[0].status=200;fixture.requests[0].responseText=' + json.dumps(json.dumps(command(50))) + ';late()')
    assert await js('fixture.events.length') == 0
    await finish(command(1))
    assert await js('fixture.events.length') == 1
    await js('fixture.tick(250)')
    await finish('not json')
    await js('fixture.fail(true);fixture.tick(250);fixture.fail(false);fixture.tick(250)')
    await finish(command(2))
    assert await js('fixture.events.length') == 1, 'successful prior Continue remains latched after transport errors'
    await js('fixture.models(5,false,false,true);fixture.tick(250)')
    await finish({'id': 0})
    await js('fixture.tick(1000)')
    await finish(command(3, 5))
    assert await js('fixture.events.length') == 2, 'transport exception must not wedge polling'

    await fresh()
    await js('window.late=fixture.requests[0].onload;__crmlUiBridge.stop();fixture.tick(1000)')
    await js('fixture.requests[0].status=200;fixture.requests[0].responseText=' + json.dumps(json.dumps(command(1))) + ';late()')
    assert await js('fixture.events.length===0 && fixture.requests.length===1 && fixture.requests[0].aborted && fixture.timers()===0')
    await fresh()
    await js('fixture.tick(1000);fixture.stopOnTrigger()')
    await finish(command(1))
    await js('fixture.tick(1000)')
    assert await js('fixture.events.length===1 && fixture.requests.length===1 && fixture.timers()===0')
    await fresh()
    await js('fixture.tick(1000);fixture.throwTrigger()')
    await finish(command(1))
    await js('fixture.tick(250)')
    await finish(command(1))
    assert await js('fixture.events.length') == 1
    await fresh()
    await finish({'id': 0})
    await js("fixture.models(0,false,false,false);ui_stacks_program_flow_state.value='main_menu';fixture.tick(250)")
    assert (await js('fixture.requests[1].url')).endswith('/9/0/0')
    await js("window.dispatchEvent(new Event('unload'))")
    assert await js('fixture.requests[1].aborted && fixture.timers()===0')

    async def batch(items, screen=1):
        await finish(dict(id=0, generation=NONCE, action=0, screen=screen, leases=items))

    def lease(name='fixture-panel', kind=1, ttl=1000):
        return dict(kind=kind, name=name, ttl_ms=ttl)

    hidden = "getComputedStyle(document.getElementById('fixture-panel')).visibility==='hidden'"
    visible = "getComputedStyle(document.getElementById('fixture-panel')).visibility==='visible' && document.getElementById('fixture-panel').style.cssText==='visibility: visible;'"
    # Embedded renderers need not implement the complete Chromium CSSOM.
    await fresh()
    await js('delete CSSStyleDeclaration.prototype.getPropertyPriority;fixture.tick(1000)')
    await finish({'id': 0})
    await js('fixture.tick(250)')
    await finish(command(1, leases=[lease()]))
    assert await js(hidden), 'presentation must work without getPropertyPriority'
    assert await js('fixture.events.length') == 1, 'presentation must not swallow Continue'
    await js('fixture.tick(1000)')
    assert await js(visible), 'reduced CSSOM still restores on expiry'
    await fresh()
    await js("document.getElementById('fixture-panel').style.setProperty('visibility','visible','important');delete CSSStyleDeclaration.prototype.getPropertyPriority")
    await batch([lease()])
    assert await js("getComputedStyle(document.getElementById('fixture-panel')).visibility==='visible'"), 'fallback preserves inline important'
    await fresh()
    await js('fixture.tick(1000)')
    await finish({'id': 0})
    await js('fixture.tick(250);document.getElementsByClassName=function(){throw Error("renderer target lookup failed")}')
    await finish(command(1, leases=[lease(kind=2)]))
    assert await js('fixture.events.length') == 1, 'failed presentation lookup must not prevent independent Continue'
    await fresh()
    await batch([lease()])
    assert await js(hidden)
    await js('fixture.tick(250);fixture.tick(750)')
    assert await js(visible), 'lease must expire even while XHR remains pending'
    await batch([lease(kind=2)])
    assert await js(hidden)
    await js('fixture.tick(250)')
    await batch([])
    assert await js(visible), 'empty batch restores value and priority'
    await fresh()
    await js('fixture.tick(1000)')
    await batch([lease()])
    assert await js(visible), 'response latency cannot extend the native lease deadline'
    await js('fixture.tick(250);fixture.tick(500)')
    await batch([lease()])
    assert await js(hidden)
    await js('fixture.tick(500)')
    assert await js(visible), 'remaining lease duration subtracts the request round trip'
    await fresh()
    await batch([lease()])
    await js('fixture.tick(250);fixture.tick(-100)')
    await batch([lease()])
    assert await js(visible), 'clock rollback rejects lease renewal and restores prior presentation'

    await fresh()
    await js('fixture.tick(1000)')
    await finish({'id': 0})
    await js('fixture.tick(250)')
    await finish(command(1, leases=[lease()]))
    assert await js(hidden)
    await js('fixture.tick(250)')
    assert await js(hidden), 'Continue latch does not release presentation on the same screen'
    await finish(command(2, leases=[lease()]))
    assert await js('fixture.events.length') == 1
    await fresh()
    await js("document.getElementById('fixture-panel').style.setProperty('visibility','visible','important')")
    await batch([lease()])
    assert await js("getComputedStyle(document.getElementById('fixture-panel')).visibility==='visible' && document.getElementById('fixture-panel').style.getPropertyPriority('visibility')==='important'"), 'inline important declarations are preserved and cannot be leased'

    await fresh()
    await batch([lease()])
    await js("document.getElementById('fixture-panel').style.setProperty('visibility','collapse');fixture.tick(250)")
    await batch([lease()])
    assert await js("document.getElementById('fixture-panel').style.visibility==='collapse'"), 'external changes block reassertion'
    await js('fixture.tick(250)')
    await batch([])
    assert await js("document.getElementById('fixture-panel').style.visibility==='collapse'"), 'cleanup preserves external writes'

    # Validation is atomic across the complete batch, including duplicate element aliases.
    malformed = ([lease(), lease()], [lease(), lease(kind=2)], [lease(name='fixture-panel,body')],
                 [lease(ttl=0)], [lease(ttl=1001)], [lease(kind=3)], [lease(name='x'*65)],
                 [lease(name='x'+str(i)) for i in range(9)], {})
    for items in malformed:
        await fresh()
        await batch([lease()])
        await js('fixture.tick(250)')
        await batch(items)
        assert await js(visible), f'invalid batch must restore prior lease: {items}'

    for kind in (1, 2):
        await fresh()
        await js("window.original=document.getElementById('fixture-panel')")
        await batch([lease(kind=kind)])
        await js("var duplicate=document.createElement('div');duplicate.id='fixture-panel';duplicate.className='fixture-panel';document.body.appendChild(duplicate);fixture.tick(250)")
        assert await js("original.style.visibility==='visible'"), 'ambiguous target invalidates the original lease'
        await batch([lease(kind=kind)])
        assert await js("original.style.visibility==='visible'"), 'ambiguous target cannot be leased'

    await fresh()
    await js("window.original=document.getElementById('fixture-panel')")
    await batch([lease()])
    await js("original.remove();fixture.tick(250)")
    assert await js("original.style.visibility==='visible'"), 'detached original is restored'
    for replace in (True, False):
        await fresh()
        await batch([lease()])
        await js("window.original=document.getElementById('fixture-panel');window.clone=original.cloneNode(true);" +
                 ("original.replaceWith(clone)" if replace else "document.body.appendChild(clone)") + ";fixture.tick(250)")
        assert await js("getComputedStyle(clone).visibility==='visible' && original.style.visibility==='visible'"), 'cloned markers cannot retain a released visibility rule'
        assert await js("clone.style.visibility==='visible'"), 'cloning never copies an injected inline hidden style'
    await fresh()
    await batch([lease()])
    await js('fixture.models(5,false,false,true);fixture.tick(250)')
    assert await js(visible), 'screen change releases presentation'
    await batch([lease()], screen=1)
    assert await js(visible), 'stale response cannot reapply presentation'

    for stop in ("__crmlUiBridge.stop()", "window.dispatchEvent(new Event('unload'))"):
        await fresh()
        await batch([lease()])
        await js(stop)
        assert await js(visible), 'stop/unload restores presentation'
    assert not browser.errors, browser.errors


async def run(args):
    from websockets.asyncio.client import connect
    executable = args.browser or default_browser()
    if not executable:
        raise SystemExit('Provide Edge/Chromium with --browser PATH')
    payload = (ROOT / 'runtime/ui_bootstrap.html').read_text(encoding='utf-8')
    assert len(payload.encode()) <= 9800
    assert payload.count('__CRML_UI_NONCE__') == 1
    directory = ROOT / '.local/ui-bootstrap-tests' / str(time.time_ns())
    directory.mkdir(parents=True)
    fixture = directory / 'fixture.html'
    fixture.write_text(SHELL.replace('PAYLOAD', payload.replace('__CRML_UI_NONCE__', NONCE)), encoding='utf-8')
    profile = directory / 'profile'
    flags = {'creationflags': subprocess.CREATE_NO_WINDOW} if os.name == 'nt' else {}
    connection = None
    with (directory / 'browser.log').open('wb') as log:
        process = subprocess.Popen([str(executable), '--headless=new', '--no-first-run', '--no-default-browser-check',
            '--disable-background-networking', '--remote-debugging-port=0', '--remote-debugging-address=127.0.0.1',
            f'--user-data-dir={profile}', 'about:blank'], stdout=log, stderr=log, **flags)
        try:
            port_file = profile / 'DevToolsActivePort'
            deadline = time.monotonic() + 15
            while not port_file.exists():
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Headless browser failed to start')
                await asyncio.sleep(.1)
            port = int(port_file.read_text().splitlines()[0])
            with urllib.request.urlopen(f'http://127.0.0.1:{port}/json/list', timeout=5) as response:
                target = next(page for page in json.load(response) if page['type'] == 'page')
            connection = await connect(target['webSocketDebuggerUrl'])
            browser = Browser(connection)
            await browser.call('Page.enable')
            await browser.call('Runtime.enable')
            await check(browser, fixture)
            print('PASS: UI readiness, per-visit Continue, bounded presentation leases, clone restoration and XHR cleanup')
        finally:
            if connection:
                try:
                    await connection.send(json.dumps(dict(id=999999, method='Browser.close')))
                except Exception:
                    pass
                await connection.close()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path)
    asyncio.run(run(parser.parse_args()))
