"""Optional native UI frontend regression checks in headless Edge/Chromium.

Run: python tests/test_native_ui_page.py [--browser PATH]
Requires Python's websockets package and a Chromium browser. This standalone
check is not a game test: its small, self-authored menu shell models page and
input lifetimes without including any extracted game asset. Mouse actions use
CDP input dispatch and real browser hit testing, never Element.click().
Artifacts and the disposable browser profile remain under .local.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]

SHELL = """<!doctype html><html><head><meta charset="utf-8"><style>
body {margin:0;background:#171717;color:#eee;font:20px sans-serif}
.options {position:absolute;inset:0;padding:36px;pointer-events:none;opacity:0}
.options.visible {pointer-events:auto;opacity:1}
.tab-buttons {display:flex;gap:12px;height:50px}
.tab-buttons-button {padding:10px 24px;background:#333}
.menu-button--selected {pointer-events:none}
.options__content {position:relative;height:650px}
.tabs__pages {position:relative;height:100%;width:100%}
.tabs__pages__page {position:relative}
.options-checkbox,.options-slider,.options-button {display:flex;align-items:center;
 min-height:54px;width:700px;padding:8px 16px;box-sizing:border-box}
.options-checkbox__label,.options-slider__label,.options-button__label {flex:1}
.options-slider__bar {position:relative;width:220px;height:24px;background:#555}
.options-slider__bar__fill {position:absolute;inset:0;background:#ddd;pointer-events:none}
.options-slider__value {min-width:36px}
.hidden,.coh-inactive {visibility:hidden;pointer-events:none}
</style></head><body><div class="options fullscreen-layout--visible visible">
<div class="tab-buttons"><div id="stock-tab" class="tab-buttons-button menu-button menu-button--selected"
 data-bind-click="routes.menu_options.root.onTab(0)">Gameplay</div>
<div id="other-tab" class="tab-buttons-button menu-button"
 data-bind-click="routes.menu_options.root.onTab(3)">Audio</div></div>
<div class="options__content"><div class="tabs__pages"><div id="stock-page"
 class="tabs__pages__page visible">Stock options</div></div></div></div>
<script>
(function(){
 var states=['gameplay'],position=0, active=true, handlers={}, clicks=0,reject=false,rejectBack=false,stockInputs=0,stockHandlers={},lastStockState='';
 var queue=[],pushCount=0,backCount=0;
 function applyPush(value){states=states.slice(0,position+1);states.push(value);++position}
 window.stack={active:function(name){return name==='menu_options'&&active},
 state:function(){return states[position]},getStackData:function(){return states.slice()},
 has:function(name,state){return name==='menu_options'&&states.indexOf(state)>=0},
 push:function(name,value){if(name==='menu_options'){++pushCount;queue.push({type:'push',value:value})}},
 back:function(name){if(name==='menu_options'){++backCount;queue.push({type:'back'})}}};
 window.engine={on:function(name,callback){(handlers[name]||(handlers[name]=[])).push(callback)},
 off:function(name,callback){handlers[name]=(handlers[name]||[]).filter(function(x){return x!==callback})}};
 ['OnNavigateLeft','OnNavigateRight','OnSelect','OnCancel'].forEach(function(name){
  var callback=function(){if(active&&['gameplay','controls','graphics','audio','interface'].indexOf(stack.state())>=0)++stockInputs};
  stockHandlers[name]=callback;engine.on(name,callback);
 });
 window.__fixture={emit:function(name){(handlers[name]||[]).slice().forEach(function(x){x()})},
 setState:function(value){states=[value];position=0}, setActive:function(value){active=value;
  document.querySelector('.options').classList.toggle('visible',value);
  document.querySelector('.options').classList.toggle('fullscreen-layout--visible',value)},
 clone:function(selector){var old=document.querySelector(selector||'.options');old.replaceWith(old.cloneNode(true))},
 rejectPush:function(value){reject=value},
 rejectBack:function(value){rejectBack=value},lastStockState:function(){return lastStockState},
 commands:function(){return {push:pushCount,back:backCount,pending:queue.length}},
 flushOne:function(){var op=queue.shift();if(!op)return;if(op.type==='push'&&!reject)applyPush(op.value);
  if(op.type==='back'&&!rejectBack&&position>0)--position},
 externalPush:function(value){applyPush(value)},externalBack:function(){if(position>0)--position},
 handlers:function(){return Object.keys(handlers).reduce(function(n,k){return n+handlers[k].length},0)},
 stockInputs:function(){return stockInputs},stockHandlerIdentity:function(){return Object.keys(stockHandlers).every(function(k){
  return (handlers[k]||[]).filter(function(x){return x===stockHandlers[k]}).length===1})},
 stockClicks:function(){return clicks}};
 window.routes={menu_options:{root:{onTab:function(index){++clicks;lastStockState=stack.state();
  states=[index===0?'gameplay':'audio'];position=0}}}};
 document.addEventListener('click',function(e){if(e.target.id==='stock-tab')routes.menu_options.root.onTab(0);
  if(e.target.id==='other-tab')routes.menu_options.root.onTab(3)});
}());
</script>PAYLOAD</body></html>"""


class Browser:
    def __init__(self, connection):
        self.connection = connection
        self.sequence = 0
        self.errors = []

    async def call(self, method, **params):
        self.sequence += 1
        request_id = self.sequence
        await self.connection.send(json.dumps(dict(id=request_id, method=method, params=params)))
        while True:
            response = json.loads(await asyncio.wait_for(self.connection.recv(), 15))
            if response.get('method') == 'Runtime.exceptionThrown':
                self.errors.append(response['params'])
            if response.get('id') == request_id:
                if 'error' in response:
                    raise AssertionError(f'{method}: {response["error"]}')
                return response.get('result', {})

    async def evaluate(self, expression):
        response = await self.call('Runtime.evaluate', expression=expression,
                                   returnByValue=True, awaitPromise=True)
        if 'exceptionDetails' in response:
            raise AssertionError(response['exceptionDetails'])
        return response.get('result', {}).get('value')

    async def wait(self, expression):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if await self.evaluate(expression):
                return
            await asyncio.sleep(.05)
        raise AssertionError(f'Timed out: {expression}')

    async def point(self, selector, fraction=.5):
        result = await self.evaluate("""(function(){var e=document.querySelector(SELECTOR);
          if(!e)return null;var b=e.getBoundingClientRect();var x=b.left+b.width*FRACTION;
          var y=b.top+b.height/2;var hit=document.elementFromPoint(x,y);
          return {x:x,y:y,hit:!!hit&&(hit===e||e.contains(hit)),width:b.width,height:b.height};})()"""
          .replace('SELECTOR', json.dumps(selector)).replace('FRACTION', str(fraction)))
        if not result or not result['hit'] or result['width'] <= 0 or result['height'] <= 0:
            raise AssertionError(f'Control is not a real mouse target: {selector}: {result}')
        return result

    async def click(self, selector, fraction=.5):
        point = await self.point(selector, fraction)
        await self.call('Input.dispatchMouseEvent', type='mouseMoved', x=point['x'], y=point['y'])
        await self.call('Input.dispatchMouseEvent', type='mousePressed', x=point['x'], y=point['y'],
                        button='left', buttons=1, clickCount=1)
        await self.call('Input.dispatchMouseEvent', type='mouseReleased', x=point['x'], y=point['y'],
                        button='left', buttons=0, clickCount=1)
        await asyncio.sleep(.05)

    async def drag(self, selector, start=.25, end=.80):
        a, b = await self.point(selector, start), await self.point(selector, end)
        await self.call('Input.dispatchMouseEvent', type='mouseMoved', x=a['x'], y=a['y'])
        await self.call('Input.dispatchMouseEvent', type='mousePressed', x=a['x'], y=a['y'],
                        button='left', buttons=1, clickCount=1)
        await self.call('Input.dispatchMouseEvent', type='mouseMoved', x=b['x'], y=b['y'],
                        button='left', buttons=1)
        await self.call('Input.dispatchMouseEvent', type='mouseReleased', x=b['x'], y=b['y'],
                        button='left', buttons=0, clickCount=1)
        await asyncio.sleep(.05)


async def check(connection, artifact_dir):
    browser = Browser(connection)
    await browser.call('Runtime.enable')
    await browser.call('Page.enable')
    await browser.call('Emulation.setDeviceMetricsOverride', width=1280, height=900,
                       deviceScaleFactor=1, mobile=False)
    status = 'window.__crmlNativeUi.status()'

    async def reset():
        await browser.call('Page.navigate', url=(artifact_dir / 'fixture.html').as_uri())
        await browser.wait("!!document.querySelector('[data-crml-native-tab]')")
        await browser.wait(f'!{status}.active && !{status}.pending')

    async def commands(push, back, pending):
        # Check stable counts over several scan cycles, catching callback floods.
        expression = (f'__fixture.commands().push === {push} && '
                      f'__fixture.commands().back === {back} && '
                      f'__fixture.commands().pending === {pending}')
        await browser.wait(expression)
        await asyncio.sleep(.25)
        if not await browser.evaluate(expression):
            raise AssertionError(f'Command count changed without acknowledgment: {expression}')

    async def begin():
        await browser.click('[data-crml-native-tab]')
        await browser.wait(f'{status}.pending && !{status}.active')
        await commands(1, 0, 1)
        await browser.wait('stack.state() === "gameplay"')

    async def acknowledge():
        await browser.evaluate('__fixture.flushOne()')
        await asyncio.sleep(.15)

    async def assert_clean():
        await browser.wait(f'!{status}.active && !{status}.pending && stack.state() === "gameplay"')
        await browser.wait('__fixture.stockHandlerIdentity()')

    # A native stack mutation only enqueues a command. Repeated clicks must not
    # enqueue duplicate pushes while state() still reports the previous page.
    await reset()
    await begin()
    for _ in range(3):
        await browser.click('[data-crml-native-tab]')
    await commands(1, 0, 1)
    await acknowledge()
    await browser.wait(f'{status}.active')
    stock_inputs = await browser.evaluate('__fixture.stockInputs()')
    await browser.evaluate("['OnNavigateLeft','OnNavigateRight','OnSelect'].forEach(__fixture.emit)")
    await browser.wait(f'__fixture.stockInputs() === {stock_inputs}')
    await browser.click('[data-crml-native-toggle]')
    await browser.wait(f'{status}.enabled')
    await browser.drag('[data-crml-native-slider]')
    await browser.wait(f'{status}.amount >= 78 && {status}.amount <= 82')
    track = await browser.point('[data-crml-native-slider]')
    for amount in (0, 9, 10, 99, 100):
        await browser.click('[data-crml-native-slider]', max(.001, min(.999, amount / 100)))
        await browser.wait(f'{status}.amount === {amount}')
        measured = await browser.point('[data-crml-native-slider]')
        if abs(measured['x'] - track['x']) > .05 or abs(measured['width'] - track['width']) > .05:
            raise AssertionError('Slider readout digit changes moved the track')
    # Real mouse movement leaves the bar, but the owning window continues the
    # drag. Mouseup outside the bar must detach drag handlers.
    await browser.call('Input.dispatchMouseEvent', type='mousePressed', x=track['x'], y=track['y'],
                       button='left', buttons=1, clickCount=1)
    await browser.call('Input.dispatchMouseEvent', type='mouseMoved', x=track['x']+track['width'], y=track['y'], buttons=1)
    await browser.call('Input.dispatchMouseEvent', type='mouseReleased', x=track['x']+track['width'], y=track['y'],
                       button='left', buttons=0, clickCount=1)
    await browser.wait(f'{status}.amount === 100')
    await browser.call('Input.dispatchMouseEvent', type='mouseMoved', x=track['x']-track['width'], y=track['y'])
    await browser.wait(f'{status}.amount === 100')
    # A separate event-shape regression models Cohtml's view coordinates. These
    # synthetic native-coordinate events supplement the real mouse tests above.
    await browser.evaluate("""(function(){var bar=document.querySelector('[data-crml-native-slider]');
      var box=bar.getBoundingClientRect();engine.isAttached=true;
      bar.dispatchEvent(new MouseEvent('mousedown',{bubbles:true,button:0,clientX:0,screenX:box.left+box.width*.65}));
      bar.style.transform='translateX(100px)';
      window.dispatchEvent(new MouseEvent('mousemove',{clientX:0,screenX:box.left+box.width*.8}));
      window.dispatchEvent(new MouseEvent('mouseup',{clientX:0,screenX:box.left+box.width*.8}));
      bar.style.transform='';engine.isAttached=false;})()""")
    await browser.wait(f'{status}.amount >= 79 && {status}.amount <= 81')
    await browser.wait("!document.querySelector('.options-slider__bar__fill--dragging')")
    await browser.click('[data-crml-native-reset]')
    await browser.wait(f'!{status}.enabled && {status}.amount === 50')
    screenshot = await browser.call('Page.captureScreenshot', format='png')
    (artifact_dir / 'native-ui.png').write_bytes(base64.b64decode(screenshot['data']))
    await browser.evaluate("__fixture.emit('OnCancel');__fixture.emit('OnCancel')")
    await commands(1, 1, 1)
    await browser.wait('stack.state() === "crml_mods_modal"')
    await browser.wait(f'__fixture.stockInputs() === {stock_inputs}')
    await acknowledge()
    await assert_clean()
    await browser.click('#other-tab')
    await browser.wait('__fixture.stockClicks() === 1')

    # Clicking a stock tab cannot replay its route while native back is still
    # pending. Replay exactly once only after the original page is current.
    await reset()
    await begin()
    await acknowledge()
    await browser.wait(f'{status}.active')
    await browser.click('#other-tab')
    await commands(1, 1, 1)
    await browser.wait('__fixture.stockClicks() === 0')
    await acknowledge()
    await browser.wait(f'!{status}.pending && __fixture.stockClicks() === 1 && stack.state() === "audio"')
    await browser.wait('__fixture.lastStockState() === "gameplay"')

    # Cancellation before push acknowledgment must remain owned until the
    # queued push lands and exactly one corresponding back is acknowledged.
    await reset()
    await begin()
    await browser.evaluate("__fixture.emit('OnCancel');__fixture.emit('OnCancel')")
    await commands(1, 0, 1)
    await acknowledge()
    await commands(1, 1, 1)
    await acknowledge()
    await assert_clean()

    # External navigation before acknowledgment cannot abandon a queued push.
    await reset()
    await begin()
    await browser.evaluate("__fixture.setState('audio')")
    await asyncio.sleep(.2)
    await commands(1, 0, 1)
    await acknowledge()
    await commands(1, 1, 1)
    await acknowledge()
    await browser.wait(f'!{status}.active && !{status}.pending && stack.state() === "audio"')

    # cloneNode preserves marker attributes but drops listener identity. A
    # replacement during pending entry must unwind, then bind the new page.
    await reset()
    await begin()
    await browser.evaluate('__fixture.clone()')
    await asyncio.sleep(.2)
    await commands(1, 0, 1)
    await acknowledge()
    await commands(1, 1, 1)
    await acknowledge()
    await assert_clean()
    await browser.wait("document.querySelectorAll('[data-crml-native-tab]').length === 1")
    await browser.click('[data-crml-native-tab]')
    await commands(2, 1, 1)
    await acknowledge()
    await browser.wait(f'{status}.active')
    await browser.click('[data-crml-native-toggle]')
    await browser.wait(f'{status}.enabled')

    # Stop during pending entry must not remove its observer/ownership record
    # before the delayed push and delayed back both finish.
    await reset()
    await begin()
    await browser.evaluate('window.__crmlNativeUi.stop()')
    await commands(1, 0, 1)
    await browser.wait(f'!!window.__crmlNativeUi && {status}.pending')
    await acknowledge()
    await commands(1, 1, 1)
    await browser.wait('!!window.__crmlNativeUi')
    await acknowledge()
    await browser.wait('!window.__crmlNativeUi && stack.state() === "gameplay"')
    await browser.wait('__fixture.handlers() === 4 && __fixture.stockHandlerIdentity()')

    # A foreign modal above our acknowledged state owns its own lifetime.
    await reset()
    await begin()
    await acknowledge()
    await browser.wait(f'{status}.active')
    await browser.evaluate("__fixture.externalPush('foreign_modal')")
    await browser.wait(f'!{status}.active && {status}.pending')
    await browser.evaluate('window.__crmlNativeUi.stop()')
    await commands(1, 0, 0)
    await browser.wait('stack.state() === "foreign_modal"')
    await browser.evaluate('__fixture.externalBack()')
    await commands(1, 1, 1)
    await acknowledge()
    await browser.wait('!window.__crmlNativeUi && stack.state() === "gameplay"')
    await browser.wait('__fixture.handlers() === 4 && __fixture.stockHandlerIdentity()')

    # Forward history includes a state already left by external navigation.
    await reset()
    await begin()
    await acknowledge()
    await browser.wait(f'{status}.active')
    await browser.evaluate('__fixture.externalBack()')
    await browser.wait(f'!{status}.active && !{status}.pending')
    await commands(1, 0, 0)
    await browser.wait('stack.has("menu_options","crml_mods_modal")')
    await browser.evaluate('__fixture.setActive(false)')
    before = await browser.evaluate(status)
    await browser.evaluate("__fixture.emit('OnCancel')")
    after = await browser.evaluate(status)
    if before['enabled'] != after['enabled'] or before['amount'] != after['amount']:
        raise AssertionError('Hidden panel changed state')
    await browser.evaluate('window.__crmlNativeUi.stop()')
    await browser.wait('!window.__crmlNativeUi && __fixture.handlers() === 4')
    if browser.errors:
        raise AssertionError(f'Uncaught browser exceptions: {browser.errors}')
    return browser


def default_browser():
    for name in ('msedge', 'chromium', 'chromium-browser', 'google-chrome'):
        found = shutil.which(name)
        if found:
            return Path(found)
    for variable in ('ProgramFiles(x86)', 'ProgramFiles'):
        if os.environ.get(variable):
            candidate = Path(os.environ[variable]) / 'Microsoft/Edge/Application/msedge.exe'
            if candidate.is_file():
                return candidate
    return None


async def run(args):
    try:
        from websockets.asyncio.client import connect
    except ImportError as error:
        raise SystemExit('Install the optional dependency: python -m pip install websockets') from error
    browser_path = args.browser or default_browser()
    if not browser_path or not browser_path.is_file():
        raise SystemExit('Provide an installed Edge/Chromium executable using --browser PATH')
    artifact_dir = ROOT / '.local/native-ui-page-tests' / time.strftime('%Y%m%d-%H%M%S')
    artifact_dir.mkdir(parents=True, exist_ok=False)
    payload = args.payload.read_text(encoding='utf-8')
    shell = SHELL
    if args.css:
        # Optional locally extracted stylesheet stays in the ignored artifact.
        shell = shell.replace('</style>', '</style><style>' + args.css.read_text(encoding='utf-8') + '</style>', 1)
    (artifact_dir / 'fixture.html').write_text(shell.replace('PAYLOAD', payload), encoding='utf-8')
    profile = artifact_dir / 'profile'
    flags = dict(creationflags=subprocess.CREATE_NO_WINDOW) if os.name == 'nt' else {}
    with (artifact_dir / 'browser.log').open('wb') as log:
        process = subprocess.Popen([str(browser_path), '--headless=new', '--no-first-run',
            '--no-default-browser-check', '--remote-debugging-port=0',
            '--remote-debugging-address=127.0.0.1', f'--user-data-dir={profile}',
            '--disable-background-networking', 'about:blank'], stdout=log, stderr=log, **flags)
        browser = None
        connection = None
        try:
            port_file = profile / 'DevToolsActivePort'
            deadline = time.monotonic() + 15
            while not port_file.exists():
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Headless browser did not become ready; inspect browser.log')
                await asyncio.sleep(.1)
            port = int(port_file.read_text().splitlines()[0])
            with urllib.request.urlopen(f'http://127.0.0.1:{port}/json/list', timeout=5) as response:
                pages = json.load(response)
            target = next(page for page in pages if page['type'] == 'page')
            connection = await connect(target['webSocketDebuggerUrl'], max_size=8*1024*1024)
            browser = await check(connection, artifact_dir)
            print('PASS: native UI queued transitions, real mouse input, cancellation, clone recovery and deferred cleanup')
            print(f'Artifacts: {artifact_dir.relative_to(ROOT)}')
        except Exception:
            if connection:
                try:
                    probe = Browser(connection)
                    snapshot = await probe.call('Page.captureScreenshot', format='png')
                    (artifact_dir / 'failure.png').write_bytes(base64.b64decode(snapshot['data']))
                    state = await probe.evaluate('document.documentElement.outerHTML')
                    (artifact_dir / 'failure.html').write_text(state, encoding='utf-8')
                    diagnostics = await probe.evaluate('({ui:window.__crmlNativeUi&&window.__crmlNativeUi.status(),'
                        'commands:window.__fixture&&__fixture.commands(),state:window.stack&&stack.state()})')
                    (artifact_dir / 'failure.json').write_text(json.dumps(diagnostics, indent=2), encoding='utf-8')
                except Exception:
                    pass
            print(f'Failure artifacts: {artifact_dir.relative_to(ROOT)}')
            raise
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
    parser.add_argument('--css', type=Path, help='Optional local stylesheet; never copied into public fixtures')
    parser.add_argument('--payload', type=Path,
                        default=ROOT / 'runtime/diagnostics/native_ui_panel.html',
                        help='Authored payload to test, for reproducing an older regression')
    asyncio.run(run(parser.parse_args()))
