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
.menu-button.selection-item--selected {--foreground-color:#0d0d0d;--background-color:#e8e8e8;
 color:var(--foreground-color);background-color:var(--background-color)}
.tab-buttons-button__icon {display:inline-block;width:12px;height:12px;background-color:var(--foreground-color)}
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
<div class="tab-buttons"><div id="stock-tab" class="tab-buttons-button menu-button menu-button--selected selection-item--selected"
 data-bind-click="routes.menu_options.root.onTab(0)"><span class="tab-buttons-button__icon"></span>Gameplay</div>
<div id="other-tab" class="tab-buttons-button menu-button"
 data-bind-click="routes.menu_options.root.onTab(3)">Audio</div></div>
<div class="options__content"><div class="tabs__pages"><div id="stock-page"
 class="tabs__pages__page visible">Stock options</div></div></div></div>
<script>
(function(){
 var states=['gameplay'],position=0, active=true,handlers={},clicks=0,stockInputs=0,lastStockState='';
 var queue=[],pushCount=0,backCount=0,worldInputs=0,foreignInputs=0;
 var normal=['gameplay','controls','graphics','audio','interface'];
 function applyPush(value){states=states.slice(0,position+1);states.push(value);++position}
 window.stack={active:function(name){return name==='menu_options'&&active},
 state:function(){return states[position]},getStackData:function(){return states.slice()},
 has:function(name,state){return name==='menu_options'&&states.indexOf(state)>=0},
 push:function(name,value){if(name==='menu_options'){++pushCount;queue.push({type:'push',value:value})}},
 back:function(name){if(name==='menu_options'){++backCount;queue.push({type:'back'})}}};
 window.engine={on:function(name,callback){(handlers[name]||(handlers[name]=[])).push(callback)},
 off:function(name,callback){if(typeof callback!=='function')throw Error('Removing whole event forbidden');
 handlers[name]=(handlers[name]||[]).filter(function(x){return x!==callback})}};
 var pairs=[['OnCancel','onCancel'],['OnPrev','onPrevTab'],['OnNext','onNextTab'],
 ['OnNavigateLeft','onNavigateLeft'],['OnNavigateRight','onNavigateRight'],['OnSelect','onSelectOption']];
 var root={onTab:function(index){++clicks;lastStockState=stack.state();states=[index===0?'gameplay':'audio'];position=0}};
 function stockOn(){pairs.forEach(function(p){engine.on(p[0],root[p[1]])})}
 function stockOff(){pairs.forEach(function(p){engine.off(p[0],root[p[1]])})}
 function setActive(value){if(active===value)return;active=value;
  if(value){states=['gameplay'];position=0;stockOn()}else stockOff();
  document.querySelector('.options').classList.toggle('visible',value);
  document.querySelector('.options').classList.toggle('fullscreen-layout--visible',value)}
 pairs.forEach(function(p){root[p[1]]=function(){if(active&&normal.indexOf(stack.state())>=0){
  ++stockInputs;if(p[0]==='OnCancel')setActive(false)}}});
 stockOn();
 // Independent listeners must remain registered throughout the panel lifetime.
 var foreign=function(){++foreignInputs};engine.on('OnSelect',foreign);
 window.routes={menu_options:{root:root}};
 window.__fixture={emit:function(name){(handlers[name]||[]).slice().forEach(function(x){x()})},
 setState:function(value){states=[value];position=0},setActive:setActive,
 clone:function(selector){var old=document.querySelector(selector||'.options');old.replaceWith(old.cloneNode(true))},
 lastStockState:function(){return lastStockState},commands:function(){return {push:pushCount,back:backCount,pending:queue.length}},
 flushOne:function(){var op=queue.shift();if(!op)return;if(op.type==='push')applyPush(op.value);
  if(op.type==='back'&&position>0)--position},
 attemptWorldInput:function(){if(!active||normal.indexOf(stack.state())<0)++worldInputs},
 worldInputs:function(){return worldInputs},foreignInputs:function(){return foreignInputs},
 handlers:function(){return Object.keys(handlers).reduce(function(n,k){return n+handlers[k].length},0)},
 stockInputs:function(){return stockInputs},stockHandlerIdentity:function(){return pairs.every(function(p){
  return (handlers[p[0]]||[]).filter(function(x){return x===root[p[1]]}).length===(active?1:0)})},
 stockClicks:function(){return clicks}};
 document.addEventListener('click',function(e){if(e.target.id==='stock-tab')root.onTab(0);
  if(e.target.id==='other-tab')root.onTab(3)});
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

    async def assert_native_owner():
        value = await browser.evaluate('__fixture.commands()')
        if value != dict(push=0, back=0, pending=0):
            raise AssertionError(f'Mods changed native Options history/input ownership: {value}')
        await browser.wait('stack.active("menu_options") && stack.state() === "gameplay"')
        await browser.evaluate('__fixture.attemptWorldInput()')
        await browser.wait('__fixture.worldInputs() === 0')

    async def open_mods():
        await browser.click('[data-crml-native-tab]')
        await assert_native_owner()
        await browser.wait(f'{status}.active && !{status}.pending')

    async def assert_clean():
        await browser.wait(f'!{status}.active && !{status}.pending')
        await browser.wait('__fixture.stockHandlerIdentity()')
        await browser.wait("!document.querySelector('[data-crml-native-muted]')")

    await reset()
    await open_mods()
    for _ in range(3):
        await browser.click('[data-crml-native-tab]')
    await assert_native_owner()
    await browser.wait('__fixture.handlers() === 2')
    stock_inputs = await browser.evaluate('__fixture.stockInputs()')
    await browser.evaluate("['OnPrev','OnNext','OnNavigateLeft','OnNavigateRight','OnSelect'].forEach(__fixture.emit)")
    await browser.wait(f'__fixture.stockInputs() === {stock_inputs} && __fixture.foreignInputs() === 1')
    # Native binding classes stay intact, but their visual selection and icon
    # colors are muted only while Mods owns the visible page.
    await browser.wait("document.querySelector('#stock-tab').classList.contains('selection-item--selected')")
    await browser.wait("getComputedStyle(document.querySelector('#stock-tab')).backgroundColor === 'rgba(0, 0, 0, 0)'")
    await browser.wait("getComputedStyle(document.querySelector('#stock-tab .tab-buttons-button__icon')).backgroundColor === 'rgb(232, 232, 232)'")
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
    await assert_clean()
    await assert_native_owner()
    await browser.wait(f'__fixture.stockInputs() === {stock_inputs}')
    await browser.wait('__fixture.handlers() === 7')
    await browser.wait("getComputedStyle(document.querySelector('#stock-tab')).backgroundColor === 'rgb(232, 232, 232)'")

    # A stock tab mouse click routes once, after stock controls are restored.
    await open_mods()
    await browser.click('#other-tab')
    await browser.wait(f'!{status}.active && __fixture.stockClicks() === 1 && stack.state() === "audio"')
    await browser.wait('__fixture.lastStockState() === "gameplay" && __fixture.stockHandlerIdentity()')

    # Closing Options outside the panel must not restore its callbacks into
    # gameplay. Reopening runs the normal native lifecycle exactly once.
    await reset()
    await open_mods()
    await browser.evaluate('__fixture.setActive(false)')
    await assert_clean()
    await browser.wait('__fixture.handlers() === 1')
    await browser.evaluate("__fixture.emit('OnSelect')")
    await browser.wait('__fixture.stockInputs() === 0 && __fixture.foreignInputs() === 1')
    await browser.evaluate('__fixture.setActive(true)')
    await browser.wait('__fixture.handlers() === 7 && __fixture.stockHandlerIdentity()')
    await open_mods()
    await browser.click('[data-crml-native-back]')
    await assert_clean()

    # The native menu may change state independently. Do not keep the local
    # selection active or navigate back over that change.
    await open_mods()
    await browser.evaluate("__fixture.setState('foreign_modal')")
    await assert_clean()
    await browser.wait('stack.state() === "foreign_modal" && __fixture.commands().back === 0')

    # Cached DOM clones retain visual attributes but not listener identity.
    await reset()
    await open_mods()
    await browser.evaluate('__fixture.clone()')
    await assert_clean()
    await browser.wait("document.querySelectorAll('[data-crml-native-tab]').length === 1")
    await open_mods()
    await browser.click('[data-crml-native-toggle]')
    await browser.wait(f'{status}.enabled')
    await browser.evaluate('window.__crmlNativeUi.stop()')
    await browser.wait('!window.__crmlNativeUi && stack.state() === "gameplay"')
    await browser.wait('__fixture.handlers() === 7 && __fixture.stockHandlerIdentity()')
    await browser.wait("!document.querySelector('[data-crml-native-muted]')")

    # Missing API support fails before hiding native controls or claiming input.
    await reset()
    await browser.evaluate('delete routes.menu_options.root.onSelectOption')
    await browser.click('[data-crml-native-tab]')
    await browser.wait(f'!{status}.active && {status}.error.length > 0')
    await assert_native_owner()
    await browser.wait('__fixture.handlers() === 7')

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
            print('PASS: native Options ownership, selection restoration, scoped callbacks, mouse controls and clone cleanup')
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
