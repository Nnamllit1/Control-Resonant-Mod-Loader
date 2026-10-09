"""Native UI frontend regression checks in headless Edge/Chromium.

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
</script><script>
window.__settingsFixture={enabled:0,amount:50};
var settingsRevisions={enabled:1,amount:1};
window.XMLHttpRequest=function(){};
XMLHttpRequest.prototype.open=function(method,url){this.url=url;};
XMLHttpRequest.prototype.abort=function(){this.aborted=true;};
XMLHttpRequest.prototype.send=function(){var self=this;setTimeout(function(){if(self.aborted)return;
var f=self.url.split('/v1/')[1].split('/'),result=0;
if(f.length===6){var key=f[3]==='1'?'enabled':'amount';if(String(settingsRevisions[key])!==f[4])result=-4;
else{__settingsFixture[key]=Number(f[5]);settingsRevisions[key]++;result=1;}}
self.status=200;self.responseText=JSON.stringify({result:result,groups:[{owner:'1',id:'fixture',items:
['enabled','amount'].map(function(key,index){return {handle:index+1,key:key,label:key,description:'',kind:index?2:1,
minimum:0,maximum:index?100:1,step:1,initial:index?50:0,value:__settingsFixture[key],revision:String(settingsRevisions[key])};})}]});self.onload();},10);};
</script>
PAYLOAD</body></html>"""


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

    async def wait(self, expression, timeout=5):
        deadline = time.monotonic() + timeout
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
        await browser.wait("!!document.querySelector('[data-crml-setting=enabled]')")

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
        await browser.wait("!!document.querySelector('[data-crml-setting=enabled]')")
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
    await browser.click('[data-crml-setting=enabled]')
    await browser.wait(f'window.__settingsFixture.enabled')
    await browser.drag('[data-crml-setting=amount] .options-slider__bar')
    await browser.wait(f'window.__settingsFixture.amount >= 78 && window.__settingsFixture.amount <= 82')
    track = await browser.point('[data-crml-setting=amount] .options-slider__bar')
    for amount in (0, 9, 10, 99, 100):
        await browser.click('[data-crml-setting=amount] .options-slider__bar', max(.001, min(.999, amount / 100)))
        await browser.wait(f'window.__settingsFixture.amount === {amount}')
        measured = await browser.point('[data-crml-setting=amount] .options-slider__bar')
        if abs(measured['x'] - track['x']) > .05 or abs(measured['width'] - track['width']) > .05:
            raise AssertionError('Slider readout digit changes moved the track')
    # Real mouse movement leaves the bar, but the owning window continues the
    # drag. Mouseup outside the bar must detach drag handlers.
    await browser.call('Input.dispatchMouseEvent', type='mousePressed', x=track['x'], y=track['y'],
                       button='left', buttons=1, clickCount=1)
    await browser.call('Input.dispatchMouseEvent', type='mouseMoved', x=track['x']+track['width'], y=track['y'], buttons=1)
    await browser.call('Input.dispatchMouseEvent', type='mouseReleased', x=track['x']+track['width'], y=track['y'],
                       button='left', buttons=0, clickCount=1)
    await browser.wait(f'window.__settingsFixture.amount === 100')
    await browser.call('Input.dispatchMouseEvent', type='mouseMoved', x=track['x']-track['width'], y=track['y'])
    await browser.wait(f'window.__settingsFixture.amount === 100')
    # Server state can already equal the requested endpoint before the UI has
    # consumed its acknowledgement. Wait for the control to become editable
    # before starting another gesture; production correctly rejects busy rows.
    await browser.wait("document.querySelector('[data-crml-setting=amount]').getAttribute('aria-disabled') === 'false'")
    # A separate event-shape regression models Cohtml's view coordinates. These
    # synthetic native-coordinate events supplement the real mouse tests above.
    await browser.evaluate("""(function(){var bar=document.querySelector('[data-crml-setting=amount] .options-slider__bar');
      var box=bar.getBoundingClientRect();engine.isAttached=true;
      bar.dispatchEvent(new MouseEvent('mousedown',{bubbles:true,button:0,clientX:0,screenX:box.left+box.width*.65}));
      if(!bar.querySelector('.options-slider__bar__fill--dragging'))throw Error('Native-coordinate gesture did not acquire slider');
      bar.style.transform='translateX(100px)';
      window.dispatchEvent(new MouseEvent('mousemove',{clientX:0,screenX:box.left+box.width*.8}));
      window.dispatchEvent(new MouseEvent('mouseup',{clientX:0,screenX:box.left+box.width*.8}));
      bar.style.transform='';engine.isAttached=false;})()""")
    await browser.wait(f'window.__settingsFixture.amount >= 79 && window.__settingsFixture.amount <= 81')
    await browser.wait("!document.querySelector('.options-slider__bar__fill--dragging')")
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
    await browser.click('[data-crml-setting=enabled]')
    await browser.wait(f'window.__settingsFixture.enabled')
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

    await browser.call('Page.navigate', url=(artifact_dir / 'settings-fixture.html').as_uri())
    await browser.wait("document.getElementById('result') && /^(PASS|FAIL):/.test(document.getElementById('result').textContent)", timeout=25)
    settings_result=await browser.evaluate("document.getElementById('result').textContent")
    if not settings_result.startswith('PASS:'):
        raise AssertionError(settings_result)
    print(settings_result)
    # Deliver real browser mouse/keyboard input as well as the fixture's
    # deterministic event-order/race probes. This validates browser editing;
    # embedded Cohtml routing still requires the documented native check.
    await browser.call('Page.navigate', url=(artifact_dir / 'settings-fixture.html').as_uri()+'?manual=1')
    await browser.wait('window.manualReady === true')
    await browser.click('[data-crml-native-tab]')
    await browser.click('[data-crml-setting="marker_name"] input')

    async def press(key, code, modifiers=0):
        await browser.call('Input.dispatchKeyEvent', type='rawKeyDown', key=key,
                           windowsVirtualKeyCode=code, modifiers=modifiers)
        await browser.call('Input.dispatchKeyEvent', type='keyUp', key=key,
                           windowsVirtualKeyCode=code, modifiers=modifiers)

    # The mod picker uses authored controls because the embedded renderer has
    # no reliable native select popup. Exercise real focus, typing and hit tests.
    await browser.click('[data-crml-mod-trigger]')
    for width, height in ((1280, 900), (1920, 1080)):
        await browser.call('Emulation.setDeviceMetricsOverride', width=width, height=height,
                           deviceScaleFactor=1, mobile=False)
        picker_geometry = await browser.evaluate('''(() => {
            const nav=document.querySelector('[data-crml-mod-navigation]'),root=document.querySelector('[data-crml-native-ui]');
            const trigger=nav.querySelector('[data-crml-mod-trigger]'),chevron=trigger.querySelector('.crml-mod-chevron');
            const input=nav.querySelector('[data-crml-mod-search]'),results=nav.querySelector('[data-crml-mod-results]');
            const a=trigger.getBoundingClientRect(),b=input.getBoundingClientRect(),c=results.getBoundingClientRect();
            const heading=nav.nextElementSibling.getBoundingClientRect(),t=getComputedStyle(trigger),s=getComputedStyle(input);
            return {width:b.width,trigger:a.width,root:root.getBoundingClientRect().width,after:b.top>=a.bottom-.5,
                resultsAfter:c.top>=b.bottom-.5,clear:c.bottom<=heading.top+.5,focused:document.activeElement===input,
                family:s.fontFamily,style:s.fontStyle,name:trigger.textContent,
                chevron:!!chevron&&getComputedStyle(chevron).borderRightWidth!=='0px'&&getComputedStyle(chevron).transform!=='none',
                triggerBorder:t.borderLeftWidth==='0px'&&t.borderTopWidth==='0px'&&t.borderRightWidth==='0px',
                searchBorder:s.borderLeftWidth==='0px'&&s.borderTopWidth==='0px'&&s.borderRightWidth==='0px',
                resultBorder:getComputedStyle(results).borderLeftWidth==='0px',
                secondary:[...results.querySelectorAll('[data-crml-mod-option="first-mod"] span')].map(x=>x.textContent).join('|')==='First <mod>|first-mod'};
        })()''')
        if (picker_geometry['width'] < 260 or picker_geometry['width'] > picker_geometry['root']*.65
                or abs(picker_geometry['width']-picker_geometry['trigger']) > 2
                or not all(picker_geometry[key] for key in ('after', 'resultsAfter', 'clear', 'focused',
                    'chevron', 'triggerBorder', 'searchBorder', 'resultBorder', 'secondary'))
                or picker_geometry['name'] != 'First <mod>'
                or 'AlteredGrotesk' not in picker_geometry['family'] or picker_geometry['style'] != 'normal'):
            raise AssertionError(f'Mod picker native row geometry at {width}x{height}: {picker_geometry}')
        await browser.point('[data-crml-mod-search]')
        await browser.point('[data-crml-mod-option="first-mod"]')
        shot = await browser.call('Page.captureScreenshot', format='png')
        (artifact_dir / f'mod-picker-{width}.png').write_bytes(base64.b64decode(shot['data']))
    await browser.call('Emulation.setDeviceMetricsOverride', width=1280, height=900,
                       deviceScaleFactor=1, mobile=False)
    picker_colors = await browser.evaluate('''(() => {
        const chosen=document.querySelector('[data-crml-mod-option="first-mod"]');
        const other=document.querySelector('[data-crml-mod-option="second-mod"]');
        return [getComputedStyle(chosen).backgroundColor,getComputedStyle(other).backgroundColor];
    })()''')
    if picker_colors[0] == picker_colors[1]:
        raise AssertionError(f'Picker selected row lacks native-style state: {picker_colors}')
    other_point = await browser.point('[data-crml-mod-option="second-mod"]')
    await browser.call('Input.dispatchMouseEvent', type='mouseMoved', x=other_point['x'], y=other_point['y'])
    hover_color = await browser.evaluate("getComputedStyle(document.querySelector('[data-crml-mod-option=second-mod]')).backgroundColor")
    if hover_color == picker_colors[1]:
        raise AssertionError('Picker hover state is not visible')
    await browser.evaluate("window.selectorKeys=0;document.addEventListener('keydown',()=>selectorKeys++)")
    await browser.call('Input.insertText', text='SECOND-MOD')
    await browser.wait("document.querySelectorAll('[data-crml-mod-option]').length===1 && document.querySelector('[data-crml-mod-option]').getAttribute('data-crml-mod-option')==='second-mod'")
    await browser.evaluate("window.modSearch=document.querySelector('[data-crml-mod-search]');window.modOption=document.querySelector('[data-crml-mod-option]');modSearch.setSelectionRange(2,5);window.modPolls=polls")
    await browser.wait('polls>=modPolls+3')
    await browser.wait("document.activeElement===modSearch && modSearch.selectionStart===2 && modSearch.selectionEnd===5 && document.querySelector('[data-crml-mod-option]')===modOption && selectorKeys===0")
    await press('ArrowDown', 40)
    await browser.wait("document.activeElement.getAttribute('data-crml-mod-option')==='second-mod'")
    await browser.evaluate('serverGroups.reverse()')
    await browser.wait("document.activeElement.getAttribute('data-crml-mod-option')==='second-mod' && document.querySelector('[data-crml-mod-navigation]').textContent.includes('MOD 2 OF 2')")
    await press('Enter', 13)
    await browser.wait("document.querySelector('[data-crml-setting=integer]') && document.querySelector('[data-crml-mod-trigger]').textContent.includes('second-mod') && __crmlNativeUi.status().active")
    await browser.evaluate('serverGroups.reverse()')
    await browser.wait("document.querySelector('[data-crml-mod-navigation]').textContent.includes('MOD 2 OF 2')")
    await browser.click('[data-crml-mod-trigger]')
    await browser.click('[data-crml-mod-search]')
    await press('a', 65, 2)
    await browser.call('Input.insertText', text='FIRST <MOD>')
    await browser.wait("document.querySelectorAll('[data-crml-mod-option]').length===1 && document.querySelector('[data-crml-mod-option]').getAttribute('data-crml-mod-option')==='first-mod'")
    await browser.click('[data-crml-mod-option="first-mod"]')
    await browser.wait("document.querySelector('[data-crml-setting=marker_name]') && document.querySelector('[data-crml-mod-trigger]').textContent.includes('First <mod>')")
    await browser.click('[data-crml-mod-trigger]')
    await browser.evaluate("serverGroups.reverse()")
    await browser.wait("document.querySelector('[data-crml-mod-trigger]').textContent.includes('First <mod>') && document.querySelector('[data-crml-mod-navigation]').textContent.includes('MOD 2 OF 2')")
    await browser.evaluate("serverGroups.reverse()")
    await browser.wait("document.querySelector('[data-crml-mod-navigation]').textContent.includes('MOD 1 OF 2')")
    await browser.click('[data-crml-mod-search]')
    await press('a', 65, 2)
    await browser.call('Input.insertText', text='no matching id')
    await browser.wait("!document.querySelector('[data-crml-mod-option]') && document.querySelector('[data-crml-mod-results] [role=status]').textContent==='No matching mods.'")
    await press('Escape', 27)
    await browser.wait("document.querySelector('[data-crml-mod-trigger]').getAttribute('aria-expanded')==='false' && __crmlNativeUi.status().active")
    await browser.click('[data-crml-mod-trigger]')
    await browser.evaluate("callbacks.OnCancel.slice().forEach(function(f){f()})")
    await browser.wait("document.querySelector('[data-crml-mod-trigger]').getAttribute('aria-expanded')==='false' && __crmlNativeUi.status().active")
    await browser.click('[data-crml-mod-trigger]')
    await browser.evaluate("for(var i=0;i<20;i++)serverGroups.push({owner:String(100+i),id:'extra-'+i,name:'Extra '+i,items:[]})")
    await browser.click('[data-crml-mod-search]')
    await press('a', 65, 2)
    await press('Backspace', 8)
    await browser.wait("document.querySelectorAll('[data-crml-mod-option]').length===22 && document.querySelector('[data-crml-mod-results]').scrollHeight>document.querySelector('[data-crml-mod-results]').clientHeight")
    await browser.evaluate("serverGroups.splice(2)")
    await browser.wait("document.querySelectorAll('[data-crml-mod-option]').length===2 && document.querySelector('[data-crml-mod-trigger]').textContent.includes('First <mod>')")
    await press('Escape', 27)
    await browser.click('[data-crml-mod-trigger]')
    await browser.evaluate("window.selectorKeyUps=0;document.addEventListener('keyup',()=>selectorKeyUps++)")
    for key, code in (('q', 81), ('e', 69), ('p', 80), ('x', 88)):
        await press(key, code)
    await browser.wait("selectorKeys===0 && selectorKeyUps===0 && stockCalls===0 && __crmlNativeUi.status().active && document.activeElement===document.querySelector('[data-crml-mod-search]')")
    await browser.evaluate("callbacks.OnCancel.slice().forEach(function(f){f()})")
    await browser.wait("document.querySelector('[data-crml-mod-trigger]').getAttribute('aria-expanded')==='false' && document.activeElement!==document.querySelector('[data-crml-mod-search]') && __crmlNativeUi.status().active")
    await asyncio.sleep(.05)  # The first Cancel's same-frame guard expires.
    await browser.evaluate("callbacks.OnCancel.slice().forEach(function(f){f()})")
    await browser.wait("!__crmlNativeUi.status().active && callbacks.OnCancel.includes(route.onCancel) && document.activeElement!==document.querySelector('[data-crml-mod-search]')")
    await browser.click('[data-crml-native-tab]')
    await browser.wait("__crmlNativeUi.status().active && document.querySelector('[data-crml-mod-search]').value==='' && document.querySelector('[data-crml-mod-trigger]').getAttribute('aria-expanded')==='false'")
    print('PASS: mod picker name/ID search, direct keyboard selection, bounded list, empty result, Q/E/P/X shielding, scoped Cancel, refresh persistence and stable caret')

    await browser.click('[data-crml-setting="marker_name"] input')
    await press('a', 65, 2)  # Ctrl+A selects the actual focused input.
    await browser.call('Input.insertText', text='é😀é')
    await browser.wait('document.activeElement.value === "é😀é" && writes === 0')
    await press('Enter', 13)
    await browser.wait('textItem.value === "é😀é" && writes === 1')
    await browser.wait('!document.activeElement.readOnly')
    await press('a', 65, 2)
    await browser.call('Input.insertText', text='Unsaved')
    await press('Escape', 27)
    await browser.wait('document.activeElement.value === "é😀é" && writes === 1 && __crmlNativeUi.status().active')
    await press('a', 65, 2)
    await browser.call('Input.insertText', text='Applied')
    await press('Tab', 9)
    await browser.wait('document.activeElement.tagName === "BUTTON" && writes === 1')
    await browser.click('[data-crml-setting="marker_name"] button')
    await browser.wait('textItem.value === "Applied" && writes === 2')
    print('PASS: browser mouse focus, Unicode insertion, Enter, Escape, Tab blur and Apply')
    # Representative stock label rules in the fixture set fixed vh typography.
    # Verify actual computed title, main-label and detail sizes, not just the
    # ancestor's inline style, at both supported font-scale endpoints.
    await browser.evaluate("document.querySelector('.options__content').classList.add('decorated-ancestor')")
    for revision, scale in (('41', .75), ('42', 1.5)):
        await browser.evaluate(f'serverGroups[0].list.font_scale={scale}; serverGroups[0].list.revision={json.dumps(revision)}')
        await browser.wait(f"Math.abs(parseFloat(getComputedStyle(document.querySelector('[data-crml-list]')).fontSize)-innerHeight*.017*{scale})<.02")
        typography = await browser.evaluate('''(() => {
            const list=document.querySelector('[data-crml-list]'),row=list.querySelector('[data-crml-list-row]');
            const values=[list.firstElementChild,row.children[0],row.children[1]].map(node=>{
                const style=getComputedStyle(node);return [parseFloat(style.fontSize),parseFloat(style.lineHeight),style.fontFamily,style.fontWeight,style.fontStyle,style.letterSpacing,style.textShadow,style.textTransform];
            });
            return {height:innerHeight,values,stock:parseFloat(getComputedStyle(document.querySelector('[data-crml-setting="amount"] .options-slider__label')).fontSize)};
        })()''')
        base = typography['height']*.017*scale
        for actual, factor in zip(typography['values'], (1, 1, .85)):
            if abs(actual[0]-base*factor)>.02 or abs(actual[1]-base*factor*1.3)>.02:
                raise AssertionError(f'List typography at scale {scale}: {typography}')
            if ('AlteredGrotesk' not in actual[2] or actual[3]!='400' or actual[4]!='normal'
                    or actual[5] not in ('normal','0px') or actual[6]!='none' or actual[7]!='none'):
                raise AssertionError(f'List inherited decorated typography at scale {scale}: {typography}')
        if abs(typography['stock']-typography['height']*.022222222222)>.02:
            raise AssertionError('List typography override changed ordinary settings labels')
    print('PASS: computed list title/label/detail font sizes and line heights at .75 and 1.5 under stock typography')
    await browser.evaluate("document.querySelector('[data-crml-list-row]').scrollIntoView({block:'nearest'})")
    await browser.click('[data-crml-list-row="18446744073709551614"]')
    await browser.wait('listAccepted.length === 1')
    await press('ArrowDown', 40)
    await browser.wait('document.activeElement.getAttribute("data-crml-list-row") === "18446744073709551612" && document.activeElement.classList.contains("options-button--focused")')
    await press('Enter', 13)
    await browser.wait('listAccepted.length === 2')
    await press(' ', 32)
    await browser.wait('listAccepted.length === 3')
    await browser.wait('listWrites === 3')
    await press('Escape', 27)
    await browser.wait('!__crmlNativeUi.status().active && document.activeElement.tagName !== "BUTTON" && stockCalls === 0')
    print('PASS: browser list mouse selection, arrow navigation, disabled skip, Enter/Space and scoped Escape')
    # A full catalog must scroll independently of Search. Use the stock flex
    # widths and a hostile inherited font style, not a simplified text-only shell.
    await browser.evaluate('installLongListFixture()')
    await browser.click('[data-crml-native-tab]')
    await browser.wait("document.querySelector('[data-crml-setting=search]') && document.querySelectorAll('[data-crml-list-row]').length === 32")

    async def assert_search_visible():
        geometry = await browser.evaluate('''(() => {
            const input=document.querySelector('[data-crml-setting=search] input');
            const apply=document.querySelector('[data-crml-setting=search] button');
            const settings=document.querySelector('[data-crml-settings-scroll]');
            const list=document.querySelector('[data-crml-list-scroll]');
            const inside=node=>{const r=node.getBoundingClientRect(),s=settings.getBoundingClientRect();
                return r.width>20 && r.height>10 && r.top>=s.top-.5 && r.bottom<=s.bottom+.5 &&
                    r.left>=s.left-.5 && r.right<=s.right+.5 && r.top>=0 && r.bottom<=innerHeight &&
                    node.contains(document.elementFromPoint(r.x+r.width/2,r.y+r.height/2));};
            const i=input.getBoundingClientRect(),a=apply.getBoundingClientRect(),c=input.parentElement.getBoundingClientRect();
            return {input:inside(input),apply:inside(apply),inputFraction:i.width/c.width,noOverlap:i.right<=a.left,
                fixedBounds:getComputedStyle(input.parentElement).display!=='flex'&&input.style.width==='75%'&&apply.style.width==='23%',settingsTop:settings.scrollTop,
                listTop:list.scrollTop,overflow:list.scrollHeight>list.clientHeight,
                separate:settings!==list&&!settings.contains(list)&&!list.contains(settings)};
        })()''')
        if (not geometry['input'] or not geometry['apply'] or not geometry['overflow'] or not geometry['separate']
                or geometry['inputFraction']<.65 or not geometry['noOverlap'] or not geometry['fixedBounds']):
            raise AssertionError(f'Search/Apply clipping or missing independent list viewport: {geometry}')
        return geometry

    await assert_search_visible()
    control_fonts = await browser.evaluate('''(() => {
        const controls=[document.querySelector('[data-crml-setting=search] input'),document.querySelector('[data-crml-setting=search] button')];
        return controls.map(node=>{const s=getComputedStyle(node);return {family:s.fontFamily,weight:s.fontWeight,style:s.fontStyle,
            size:parseFloat(s.fontSize)/innerHeight,line:parseFloat(s.lineHeight)/parseFloat(s.fontSize),spacing:s.letterSpacing,shadow:s.textShadow,transform:s.textTransform};});
    })()''')
    for font, size in zip(control_fonts, (.022, .02)):
        if ('AlteredGrotesk' not in font['family'] or font['weight']!='400' or font['style']!='normal'
                or abs(font['size']-size)>.0001 or abs(font['line']-1.3)>.01
                or font['spacing'] not in ('normal', '0px') or font['shadow']!='none' or font['transform']!='none'):
            raise AssertionError(f'Form control inherited decorated typography: {control_fonts}')
    await browser.click('[data-crml-list-row="1"]')
    await press('End', 35)
    await browser.wait('document.activeElement.getAttribute("data-crml-list-row") === "32" && document.querySelector("[data-crml-list-scroll]").scrollTop>0')
    before = await assert_search_visible()
    await browser.evaluate("serverGroups[0].list.revision='101';serverGroups[0].list.rows[31].detail='Updated final row detail'")
    await browser.wait("document.querySelector('[data-crml-list-row=\"32\"]').textContent.includes('Updated final row detail') && document.activeElement.getAttribute('data-crml-list-row')==='32'")
    after = await assert_search_visible()
    if abs(after['listTop']-before['listTop'])>1 or after['settingsTop']!=before['settingsTop']:
        raise AssertionError(f'List revision moved independent scroll positions: {before}, {after}')
    await browser.click('[data-crml-setting=search] input')
    focus_colors=await browser.evaluate("(() => {const s=getComputedStyle(document.activeElement);return [s.borderTopColor,s.backgroundColor]})()")
    if focus_colors!=['rgb(232, 232, 232)', 'rgba(13, 13, 13, 0.9)']:
        raise AssertionError(f'Text focus contrast did not override inline colors: {focus_colors}')
    await browser.call('Input.insertText', text='Find me')
    await browser.evaluate("window.searchNode=document.activeElement;searchNode.setSelectionRange(1,4);serverGroups[0].list.revision='102';serverGroups[0].list.rows[0].label='Refreshed first result'")
    await browser.wait("document.querySelector('[data-crml-list-row=\"1\"]').textContent.includes('Refreshed first result')")
    await browser.wait('document.activeElement===searchNode && searchNode.value==="Find me" && searchNode.selectionStart===1 && searchNode.selectionEnd===4')
    await assert_search_visible()
    # Poll unchanged data and verify neither active input nor list gets replaced.
    await browser.evaluate("window.layoutPollStart=polls;window.listNode=document.querySelector('[data-crml-list]')")
    await browser.wait('polls>=layoutPollStart+3')
    await browser.wait("document.activeElement===searchNode && document.querySelector('[data-crml-list]')===listNode")
    for width, height in ((1280, 900), (1920, 1080)):
        await browser.call('Emulation.setDeviceMetricsOverride', width=width, height=height, deviceScaleFactor=1, mobile=False)
        await assert_search_visible()
        shot=await browser.call('Page.captureScreenshot',format='png')
        (artifact_dir/f'list-search-{width}.png').write_bytes(base64.b64decode(shot['data']))
    await press('Escape', 27)  # Cancel the uncommitted search before changing mods.
    await browser.evaluate("document.querySelector('[data-crml-settings-scroll]').scrollTop=10000")
    await browser.click('[data-crml-mod-navigation] > div:first-child > .options-button:last-child')
    await browser.wait("document.querySelector('[data-crml-native-ui]').textContent.includes('List 12')")
    reset_geometry = await assert_search_visible()
    if reset_geometry['settingsTop']!=0 or reset_geometry['listTop']!=0:
        raise AssertionError(f'Owner change retained the previous mod scroll: {reset_geometry}')
    await browser.click('[data-crml-list-row="1"]')
    await press('End', 35)
    await browser.wait('document.activeElement.getAttribute("data-crml-list-row") === "32"')
    await browser.click('[data-crml-mod-navigation] > div:first-child > .options-button:first-child')
    await browser.wait("document.querySelector('[data-crml-native-ui]').textContent.includes('List 11')")
    reset_geometry = await assert_search_visible()
    if reset_geometry['settingsTop']!=0 or reset_geometry['listTop']!=0:
        raise AssertionError(f'Returning to a mod retained stale scroll: {reset_geometry}')
    print('PASS: explicit control typography, 32-row independent scrolling, Search hit testing/draft focus, revision stability and owner scroll reset at two viewports')
    await browser.call('Page.navigate', url=(artifact_dir / 'drawing-fixture.html').as_uri())
    await browser.wait("document.getElementById('result') && /^(PASS|FAIL):/.test(document.getElementById('result').textContent)", timeout=25)
    drawing_result = await browser.evaluate("document.getElementById('result').textContent")
    if not drawing_result.startswith('PASS:'):
        raise AssertionError(drawing_result)
    print(drawing_result)
    await browser.call('Page.navigate', url=(artifact_dir / 'drawing-fixture.html').as_uri()+'?visual=1')
    await browser.wait('window.visualReady === true')
    for width, height in ((1280, 900), (1920, 1080)):
        await browser.call('Emulation.setDeviceMetricsOverride', width=width, height=height, deviceScaleFactor=1, mobile=False)
        await browser.wait("Math.abs(document.querySelector('[data-crml-drawing-segment]').getBoundingClientRect().width-(innerWidth*.1+innerHeight*.002))<1")
        shot=await browser.call('Page.captureScreenshot',format='png')
        (artifact_dir/f'drawing-{width}.png').write_bytes(base64.b64decode(shot['data']))
        pixels = await browser.evaluate('''(async function(url) {
            const image=new Image();await new Promise((ok,fail)=>{image.onload=ok;image.onerror=fail;image.src=url;});
            const canvas=document.createElement('canvas');canvas.width=image.width;canvas.height=image.height;
            const context=canvas.getContext('2d');context.drawImage(image,0,0);
            const data=context.getImageData(0,0,canvas.width,canvas.height).data;
            let count=0;for(let i=0;i<data.length;i+=4)if(data[i]>250&&Math.abs(data[i+1]-170)<3&&data[i+2]<3)count++;
            return count;
        })('''+json.dumps('data:image/png;base64,'+shot['data'])+')')
        expected = 16 * width * .1 * height * .002
        if not expected*.6 < pixels < expected*1.4:
            raise AssertionError(f'Drawing painted pixels at {width}x{height}: {pixels}, expected about {expected}')
    print('PASS: drawing painted segment pixels at 1280x900 and 1920x1080')
    await browser.call('Page.navigate', url=(artifact_dir / 'drawing-fixture.html').as_uri()+'?visual=joins')
    await browser.wait('window.visualReady === true')
    for width, height in ((1280, 900), (1920, 1080)):
        await browser.call('Emulation.setDeviceMetricsOverride', width=width, height=height, deviceScaleFactor=1, mobile=False)
        await browser.wait("document.querySelectorAll('[data-crml-drawing-segment]').length===5 && Math.abs(document.querySelector('[data-crml-drawing-segment]').getBoundingClientRect().width-(innerWidth*.2+innerHeight*.006))<1")
        shot=await browser.call('Page.captureScreenshot',format='png')
        (artifact_dir/f'drawing-joins-{width}.png').write_bytes(base64.b64decode(shot['data']))
        painted = await browser.evaluate('''(async function(url) {
            const image=new Image();await new Promise((ok,fail)=>{image.onload=ok;image.onerror=fail;image.src=url;});
            const canvas=document.createElement('canvas');canvas.width=image.width;canvas.height=image.height;
            const context=canvas.getContext('2d');context.drawImage(image,0,0);
            return [[.1,.2],[.5,.2],[.5,.6],[.5001,.6001],[.2,.35],[.8,.8],[.7,.6],[.65,.45]].map(function(p){
                const x=Math.round((.1+p[0]*.5)*image.width), y=Math.round((.1+p[1]*.5)*image.height);
                const d=context.getImageData(x,y,1,1).data;return d[0]>250&&Math.abs(d[1]-170)<3&&d[2]<3;
            });
        })('''+json.dumps('data:image/png;base64,'+shot['data'])+')')
        if painted != [True]*7+[False]:
            raise AssertionError(f'Round joins/endpoints or disconnected gap at {width}x{height}: {painted}')
    print('PASS: round caps, sharp corners, subpixel segments and disconnected gaps painted correctly')
    await browser.call('Page.navigate', url=(artifact_dir / 'drawing-fixture.html').as_uri()+'?editor=1')
    await browser.wait('window.editorReady === true')
    hover=await browser.point('[data-crml-annotation-id="100"]')
    await browser.call('Input.dispatchMouseEvent', type='mouseMoved', x=hover['x'], y=hover['y'])
    await browser.wait("document.querySelector('[data-crml-marker-preview]') !== null")
    await browser.click('[data-crml-annotation-id="100"]')
    await browser.wait("document.querySelector('[data-crml-map-editor] input') && !document.querySelector('[data-crml-map-editor] input').readOnly")
    await browser.call('Input.insertText', text='Named from map')
    await browser.click('[data-crml-map-editor] [aria-label="Pink"]')
    await browser.click('[data-crml-map-editor] > div:last-child > button:first-child')
    await browser.wait("edits.length === 1 && !document.querySelector('[data-crml-map-editor]')")
    action = await browser.evaluate('edits[0]')
    parts = action.split('/edit/')[1].split('/')
    if parts[5:8] != ['2', '100', str(0xeda9ffff)] or bytes.fromhex(parts[12][1:]).decode() != 'Named from map':
        raise AssertionError(f'Map editor did not copy intended name/color: {parts}')
    if await browser.evaluate("!!document.querySelector('[data-crml-add-marker]')"):
        raise AssertionError('Duplicate placement button remains')
    await browser.evaluate('addMapBlocker()')
    await browser.wait("(()=>{let h=document.querySelector('[data-crml-native-marker=\"2\"]');if(!h)return false;let b=h.getBoundingClientRect();return document.elementFromPoint(b.left+b.width/2,b.top+b.height/2)===h})()")
    if not await browser.evaluate("document.elementFromPoint(120,100).id==='map-blocker'"):
        raise AssertionError('Marker interaction layer stole ordinary map input')
    hover=await browser.point('[data-crml-native-marker="2"]')
    await browser.call('Input.dispatchMouseEvent',type='mouseMoved',x=hover['x'],y=hover['y'])
    await browser.wait("document.querySelector('[data-crml-marker-preview].map-context-menu .map-context-menu__title') !== null")
    if not await browser.evaluate("getComputedStyle(document.querySelector('.map-context-menu[data-bind-rmd-visible]')).visibility==='hidden'"):
        raise AssertionError('Stock tooltip overlaps the marker preview')
    await browser.click('[data-crml-native-marker="2"]')
    await browser.wait("document.querySelector('[data-crml-map-editor] input') && !document.querySelector('[data-crml-map-editor] input').readOnly")
    # Simulate an engine-renderer offset in measured coordinates. Repeated
    # fitting must not feed that offset back into CSS and walk across the screen.
    await browser.evaluate("(()=>{let p=document.querySelector('[data-crml-map-editor]');window.editorRect=p.getBoundingClientRect.bind(p);p.getBoundingClientRect=()=>{let r=editorRect();return new DOMRect(r.x+7,r.y+9,r.width,r.height)};window.anchorStyle=[p.style.left,p.style.top];window.anchorCheckAt=performance.now()+1300})()")
    await browser.wait("performance.now()>anchorCheckAt")
    if not await browser.evaluate("(()=>{let p=document.querySelector('[data-crml-map-editor]');return p.style.left===anchorStyle[0]&&p.style.top===anchorStyle[1]})()"):
        raise AssertionError('Editor position feeds renderer coordinates back into CSS')
    await browser.evaluate("document.querySelector('[data-crml-map-editor]').getBoundingClientRect=editorRect")
    await browser.call('Input.insertText', text='Existing X marker')
    await browser.click('[data-crml-map-editor] [aria-label="Cyan"]')
    shot=await browser.call('Page.captureScreenshot',format='png')
    (artifact_dir/'map-marker-editor.png').write_bytes(base64.b64decode(shot['data']))
    # Force overflow as at small window sizes / larger UI scales. The body must
    # scroll to reach Save/Cancel, and wheel input must not escape.
    await browser.evaluate("document.querySelector('[data-crml-map-editor]').firstElementChild.style.minHeight='1200px';window.escapedWheel=0;window.addEventListener('wheel',()=>++window.escapedWheel)")
    await browser.wait("(()=>{let s=document.querySelector('[data-crml-editor-scroll]');let r=s.getBoundingClientRect();return s.scrollHeight>s.clientHeight&&r.top>=0&&r.bottom<=innerHeight})()")
    scroll=await browser.point('[data-crml-editor-scroll]')
    await browser.call('Input.dispatchMouseEvent',type='mouseMoved',x=scroll['x'],y=scroll['y'])
    await browser.call('Input.dispatchMouseEvent',type='mouseWheel',x=scroll['x'],y=scroll['y'],deltaX=0,deltaY=2000)
    await browser.wait("document.querySelector('[data-crml-editor-scroll]').scrollTop>0")
    if not await browser.evaluate("(()=>{let p=document.querySelector('[data-crml-map-editor]'),r=p.getBoundingClientRect(),b=p.lastElementChild.getBoundingClientRect();return !escapedWheel && r.top>=0 && r.bottom<=innerHeight && b.top>=r.top && b.bottom<=r.bottom+1 && p.querySelectorAll('input')[1].value==='Existing X marker'})()"):
        raise AssertionError('Editor scroll state: '+str(await browser.evaluate("(()=>{let p=document.querySelector('[data-crml-map-editor]');return {wheel:escapedWheel,panel:p.getBoundingClientRect().toJSON(),footer:p.lastElementChild.getBoundingClientRect().toJSON(),height:innerHeight,name:p.querySelectorAll('input')[1].value,scroll:p.scrollTop}})()")))
    await browser.evaluate("let s=document.querySelector('[data-crml-editor-scroll]');s.focus();s.dispatchEvent(new KeyboardEvent('keydown',{key:'Home',bubbles:true,cancelable:true}))")
    if not await browser.evaluate("document.querySelector('[data-crml-editor-scroll]').scrollTop===0"):
        raise AssertionError('Editor scroll region cannot be navigated by keyboard')
    shot=await browser.call('Page.captureScreenshot',format='png')
    (artifact_dir/'map-marker-editor-scroll.png').write_bytes(base64.b64decode(shot['data']))
    await browser.evaluate("let p=document.querySelector('[data-crml-map-editor]');p.scrollTop=p.scrollHeight")
    await browser.click('[data-crml-map-editor] > div:last-child > button:first-child')
    await browser.wait('edits.length === 2')
    parts=(await browser.evaluate('edits[1]')).split('/edit/')[1].split('/')
    if parts[5:7]!=['4','2'] or abs(float(parts[9])-350/600)>.001 or abs(float(parts[10])-160/500)>.001:
        raise AssertionError(f'Native marker identity/local position mismatch: {parts}')
    await browser.evaluate("serverAnnotations[0].revision='8';serverAnnotations[0].items.push({id:'2',native:true,x:350/600,y:160/500,rgba:0x7ae8ffff,distance:0,editable:true,symbol:'C',name:'Existing X marker',description:'Saved description'})")
    await browser.wait("document.querySelector('[data-crml-native-marker=\"2\"]').textContent === 'C'")
    await browser.wait("document.querySelector('[data-crml-sonar-marker=\"2\"]') && document.querySelector('[data-crml-sonar-marker=\"2\"]').textContent==='C'")
    await browser.evaluate("document.querySelector('.map-view').style.display='none'")
    await browser.wait("!document.querySelector('[data-crml-native-marker]')")
    if not await browser.evaluate("document.querySelector('[data-crml-sonar-marker=\"2\"]').textContent==='C'"):
        raise AssertionError('Closing map discarded minimap metadata')
    await browser.evaluate("window.sonarNode=document.querySelector('[data-crml-sonar-marker]');window.sonarChanges=0;window.sonarObserver=new MutationObserver(()=>++sonarChanges);sonarObserver.observe(document.querySelector('.poi__custom-marker'),{childList:true,subtree:true,attributes:true});window.sonarStableAfter=performance.now()+1400")
    await browser.wait("performance.now()>sonarStableAfter")
    if not await browser.evaluate("sonarObserver.disconnect();sonarChanges===0 && document.querySelector('[data-crml-sonar-marker]')===sonarNode"):
        raise AssertionError('Sonar-only metadata flickers or is rebuilt between renewals')
    await browser.evaluate("document.querySelector('.map-view').style.display='block'")
    await browser.wait("document.querySelector('[data-crml-native-marker=\"2\"]') && document.querySelector('[data-crml-native-marker=\"2\"]').textContent==='C'")
    await browser.evaluate("document.querySelector('.map-view__container__markers').style.transform='scale(.9)'")
    await browser.wait("(()=>{let a=document.querySelector('[data-crml-native-marker=\"2\"]').getBoundingClientRect(),b=document.querySelector('[data-test-native-id=\"2\"]').getBoundingClientRect();return Math.abs(a.left-b.left)<1&&Math.abs(a.width-b.width)<1})()")
    await browser.click('[data-crml-native-marker="2"]')
    if not await browser.evaluate("document.querySelector('[data-crml-map-editor] input').value==='C' && document.querySelectorAll('[data-crml-map-editor] input')[1].value==='Existing X marker'"):
        raise AssertionError('Saved native marker did not reopen with its metadata')
    await browser.evaluate("window.ui_map_custom_marker_1_position.value.x=390")
    await browser.wait("!document.querySelector('[data-crml-map-editor]') && document.querySelector('[data-crml-native-marker=\"2\"]').textContent === ''")
    await browser.wait("!document.querySelector('[data-crml-sonar-marker]')")
    if not await browser.evaluate("document.querySelector('.poi__custom-marker').style.backgroundColor==='orange' && document.querySelector('.poi__custom-marker .custom-marker__number').style.visibility===''"):
        raise AssertionError('Reused sonar slot retained stale decoration')
    # Relocated native slots must not inherit the previous position's name/color.
    await browser.evaluate("document.dispatchEvent(new KeyboardEvent('keydown',{key:'x',bubbles:true}))")
    await browser.wait("document.querySelector('[data-crml-native-marker=\"3\"]') !== null")
    if await browser.evaluate('stockPlacements') != 1:
        raise AssertionError('Native X placement was intercepted or duplicated')
    await browser.evaluate("window.ui_map_custom_marker_2_hidden.value=true")
    await browser.wait("!document.querySelector('[data-crml-native-marker=\"3\"]')")
    await browser.evaluate("window.ui_map_custom_marker_2_hidden.value=false")
    await browser.wait("!!document.querySelector('[data-crml-native-marker=\"3\"]')")
    await browser.click('[data-crml-native-marker="3"]')
    await browser.evaluate("document.querySelector('.map-view').style.display='none'")
    await browser.wait("!document.querySelector('[data-crml-map-editor]') && !document.querySelector('[data-crml-native-marker]')")
    if not await browser.evaluate("Array.from(document.querySelectorAll('.custom-marker')).every(n=>n.style.visibility==='')"):
        raise AssertionError('Native marker appearance not restored on teardown')
    if not await browser.evaluate("!document.documentElement.hasAttribute('data-crml-map-interaction') && getComputedStyle(document.querySelector('.map-context-menu[data-bind-rmd-visible]')).visibility!=='hidden'"):
        raise AssertionError('Stock tooltip not restored after map close')
    print('PASS: existing native marker editing, native X passthrough, slot movement invalidation and map-close restoration')
    await browser.evaluate("""(()=>{
        document.querySelector('.map-view').style.display='block';window.ui_map_custom_marker_1_position.value={x:350,y:160};
        let g=serverAnnotations[0];g.version=3;g.revision='90';g.native_markers=true;
        g.items=[{id:'2',x:.2,y:.2,rgba:0xffffffff,distance:0,editable:true,symbol:'W',name:'World two',description:''},
                 {id:'2',native:true,x:350/600,y:160/500,rgba:0xffffffff,distance:0,editable:true,symbol:'N',name:'Native two',description:''}];
    })()""")
    await browser.wait("!!document.querySelector('[data-crml-annotation-id=\"2\"]') && !!document.querySelector('[data-crml-native-marker=\"2\"]')")
    count=await browser.evaluate('edits.length')
    await browser.click('[data-crml-annotation-id="2"]')
    await browser.wait("document.querySelector('[data-crml-map-editor] input') && !document.querySelector('[data-crml-map-editor] input').readOnly")
    await browser.click('[data-crml-map-editor] > div:last-child > button:first-child')
    await browser.wait(f'edits.length === {count+1}')
    fields=(await browser.evaluate('edits[edits.length-1]')).split('/edit2/')[1].split('/')
    if fields[5:7]!=['2','2'] or fields[14]!='1':
        raise AssertionError(f'Typed world ID2 incorrectly routed: {fields}')
    await browser.click('[data-crml-native-marker="2"]')
    await browser.wait("document.querySelector('[data-crml-map-editor] input') && !document.querySelector('[data-crml-map-editor] input').readOnly")
    await browser.click('[data-crml-map-editor] > div:last-child > button:first-child')
    await browser.wait(f'edits.length === {count+2}')
    fields=(await browser.evaluate('edits[edits.length-1]')).split('/edit2/')[1].split('/')
    if fields[5:7]!=['4','2'] or fields[14]!='2':
        raise AssertionError(f'Typed native slot2 incorrectly routed: {fields}')
    print('PASS: same world/native ID edits remain separate through typed UI transport')

    await browser.evaluate("""(()=>{
        document.querySelector('.map-view').style.display='block';
        let g=serverAnnotations[0];g.revision='99';g.native_markers=false;
        g.items=Array.from({length:128},(_,i)=>({id:String(100+i),x:.05+(i%12)*.075,y:.04+Math.floor(i/12)*.075,rgba:0x7ae8ffff,distance:0,editable:true,symbol:'A',name:'Marker '+i,description:''}));
        let c=document.createElement('div');c.className='sonar-container';c.style.cssText='position:absolute;right:0;top:0;width:200px;height:200px';
        let n=document.createElement('div');n.className='sonar';n.style.cssText='position:relative;width:100%;height:100%';c.appendChild(n);document.body.appendChild(c);
        serverSonar=[{owner:'1',remaining_ms:500,items:g.items.map(a=>({id:a.id,x:a.x,y:a.y,rgba:a.rgba,symbol:a.symbol}))}];
    })()""")
    await browser.wait("document.querySelectorAll('[data-crml-annotation-id]').length===128 && document.querySelectorAll('[data-crml-extra-sonar]').length===128")
    if not await browser.evaluate("(()=>{let s=document.querySelector('[data-crml-sonar-annotations]');return s&&s.style.borderRadius==='50%'&&s.style.overflow==='hidden'&&s.style.pointerEvents==='none'})()"):
        raise AssertionError('Expanded sonar layer lacks circular clipping or input transparency')
    # Dense unchanged maps must not repeatedly invalidate layout for every marker.
    profile=await browser.call('Runtime.evaluate', expression="""new Promise(resolve=>{
        const plane=document.querySelector('.map-view__container__markers');
        const oldStyle=window.getComputedStyle,oldRect=Element.prototype.getBoundingClientRect;
        let reads=0,mutations=0,frames=0,start=performance.now();
        window.getComputedStyle=function(n){if(n===plane)++reads;return oldStyle.apply(this,arguments)};
        Element.prototype.getBoundingClientRect=function(){if(this===plane)++reads;return oldRect.apply(this,arguments)};
        const observer=new MutationObserver(records=>mutations+=records.length);
        document.querySelectorAll('[data-crml-annotation-visual],[data-crml-annotation-id],[data-crml-extra-sonar]').forEach(n=>observer.observe(n,{attributes:true,attributeFilter:['style']}));
        function tick(){if(++frames<30){requestAnimationFrame(tick);return;}
            observer.disconnect();window.getComputedStyle=oldStyle;Element.prototype.getBoundingClientRect=oldRect;
            resolve({frames,sharedAncestorReads:reads,styleMutations:mutations,elapsedMs:performance.now()-start});
        }requestAnimationFrame(tick);
    })""", awaitPromise=True, returnByValue=True)
    metrics=profile['result']['value']
    (artifact_dir/'marker-performance.json').write_text(json.dumps(metrics,indent=2),encoding='utf-8')
    if metrics['sharedAncestorReads']>metrics['frames']*30 or metrics['styleMutations']:
        raise AssertionError(f'Dense unchanged markers repeatedly invalidate layout: {metrics}')
    print('PASS: dense marker layout reads are shared; unchanged map/sonar styles remain untouched')
    hover=await browser.point('[data-crml-annotation-id="227"]')
    await browser.call('Input.dispatchMouseEvent',type='mouseMoved',x=hover['x'],y=hover['y'])
    await browser.wait("lastDrawingPoll.includes('&hover=1,99,3,227')")
    if not await browser.evaluate("(()=>{let values=lastDrawingPoll.split('&hover_box=')[1].split('&')[0].split(',').map(Number),b=document.querySelector('[data-crml-annotation-id=\"227\"]').getBoundingClientRect();return values.length===4&&Math.abs(values[0]*innerWidth-b.left)<1&&Math.abs(values[3]*innerHeight-b.bottom)<1})()"):
        raise AssertionError('Hover does not include its current client-space hit rectangle')
    await browser.call('Input.dispatchMouseEvent',type='mouseMoved',x=5,y=5)
    await browser.wait("!lastDrawingPoll.includes('&hover=')")
    await browser.call('Input.dispatchMouseEvent',type='mouseMoved',x=hover['x'],y=hover['y'])
    await browser.wait("lastDrawingPoll.includes('&hover=1,99,3,227')")
    await browser.evaluate("window.dispatchEvent(new Event('blur'))")
    await browser.wait("!lastDrawingPoll.includes('&hover=')")
    await browser.call('Input.dispatchMouseEvent',type='mouseMoved',x=5,y=5)
    print('PASS: hovered world identity is renewed and released on leave or focus loss')
    # Stock and overflow marker bodies must share size/zoom; only metadata and
    # ownership differ. Keep native slot discovery restricted to bound nodes.
    if not await browser.evaluate("""(()=>{
        let extra=document.querySelector('[data-crml-annotation-visual="227"]'),stock=document.querySelector('[data-test-native-id="2"]');
        let e=extra.getBoundingClientRect(),b=stock.getBoundingClientRect();
        return Math.abs(e.width-b.width)<.1&&Math.abs(e.height-b.height)<.1&&!!extra.querySelector('.custom-marker__number')&&extra.children.length===1;
    })()"""):
        raise AssertionError('Overflow marker dimensions/hierarchy differ from stock at close zoom')
    await browser.evaluate("window.ui_map_transforms_zoom_range.value=0")
    await browser.wait("getComputedStyle(document.querySelector('[data-crml-annotation-visual=\"227\"]')).display==='none'")
    await browser.evaluate("window.ui_map_transforms_zoom_range.value=2")
    await browser.wait("getComputedStyle(document.querySelector('[data-crml-annotation-id=\"227\"]')).display!=='none'")
    await browser.click('[data-crml-annotation-id="227"]')
    await browser.wait("document.querySelector('[data-crml-map-editor] input') && !document.querySelector('[data-crml-map-editor] input').readOnly")
    await browser.evaluate("window.lastSonarNode=document.querySelector('[data-crml-extra-sonar=\"227\"]')")
    await browser.call('Runtime.evaluate', expression='new Promise(r=>setTimeout(r,1200))', awaitPromise=True)
    if not await browser.evaluate("window.lastSonarNode===document.querySelector('[data-crml-extra-sonar=\"227\"]')"):
        raise AssertionError('Unchanged extended sonar marker is rebuilt on every poll')
    await browser.evaluate("""(()=>{
        window.routeSurface=surface(3,1,1000,false);routeSurface.target=2;routeSurface.rect=[0,0,1,1];
        server=[routeSurface];window.renewRoute=setInterval(()=>{routeSurface.expires=performance.now()+900},100);
    })()""")
    await browser.wait("!!document.querySelector('[data-crml-map-drawing=\"2\"] [data-crml-drawing-segment]')")
    await browser.evaluate("window.routeRoot=document.querySelector('[data-crml-map-drawing=\"2\"]');window.routeLine=routeRoot.firstChild;routeSurface.revision='2';routeSurface.segments[0][0]=.2")
    await browser.wait("window.routeLine.style.left==='20%'")
    if not await browser.evaluate("document.querySelector('[data-crml-map-drawing=\"2\"]')===routeRoot&&routeRoot.firstChild===routeLine"):
        raise AssertionError('Moving sonar projection rebuilds its line nodes')
    await browser.evaluate("clearInterval(window.renewRoute);server=[]")
    print('PASS: stock-sized overflow markers, native zoom visibility and in-place sonar line updates')
    await browser.evaluate("serverAnnotations=[];window.sonarOnly=document.querySelector('[data-crml-extra-sonar=\"227\"]');window.sonarMissing=false;window.sonarCheck=setInterval(()=>{if(document.querySelector('[data-crml-extra-sonar=\"227\"]')!==window.sonarOnly)window.sonarMissing=true},16)")
    await browser.call('Runtime.evaluate', expression='new Promise(r=>setTimeout(r,1800))', awaitPromise=True)
    if await browser.evaluate('clearInterval(window.sonarCheck);window.sonarMissing'):
        raise AssertionError('Map-closed extended markers blink or rebuild without a route')
    await browser.evaluate('serverAnnotations=[];serverSonar=[]')
    await browser.wait("!document.querySelector('[data-crml-annotation-id]') && !document.querySelector('[data-crml-extra-sonar]') && !document.querySelector('[data-crml-map-editor]')")
    await browser.evaluate("serverSonarActive=true;window.cadenceStart=polls")
    await browser.call('Runtime.evaluate', expression='new Promise(r=>setTimeout(r,1000))', awaitPromise=True)
    if not await browser.evaluate("polls-window.cadenceStart>=10"):
        raise AssertionError('Offscreen live sonar source fell back to idle polling')
    await browser.evaluate("serverSonarActive=false")
    print('PASS: 128 full-map hit targets, last-marker editing, stable sonar nodes and owner cleanup')

    await browser.call('Page.navigate', url=(artifact_dir / 'feedback-fixture.html').as_uri())
    await browser.wait("document.getElementById('result') && /^(PASS|FAIL):/.test(document.getElementById('result').textContent)", timeout=15)
    feedback_result=await browser.evaluate("document.getElementById('result').textContent")
    if not feedback_result.startswith('PASS:'):
        raise AssertionError(feedback_result)
    for terminal in (403, 409, 503):
        for fixture in ('feedback-fixture.html', 'settings-fixture.html', 'drawing-fixture.html'):
            await browser.call('Page.navigate', url=(artifact_dir / fixture).as_uri()+f'?terminal={terminal}')
            await browser.wait("document.getElementById('result') && /^(PASS|FAIL):/.test(document.getElementById('result').textContent)")
            result = await browser.evaluate("document.getElementById('result').textContent")
            if not result.startswith('PASS:'):
                raise AssertionError(result)

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


async def check_tutorial_layout(browser, fixture, artifact_dir):
    """Exercise the production binding and inspect rendered pixels, not img geometry."""
    await browser.call('Page.navigate', url=fixture.resolve().as_uri())
    await browser.wait('document.querySelectorAll(".case").length === 6')
    script=(ROOT/'runtime/diagnostics/native_ui_tutorials.html').read_text(encoding='utf-8').split('<script>',1)[1].split('</script>',1)[0]
    await browser.evaluate("window.engine={registerBindingAttribute:(name, Handler)=>{window.Layout=Handler}}")
    await browser.evaluate(script)
    await browser.evaluate('''(() => {
        window.handlers=[];
        const create=document.createElement.bind(document);
        // The game texture path must not depend on HTMLImageElement loading or
        // dimensions. Reject that old path even though Chromium supports it.
        document.createElement=function(tag) {
            if(tag.toLowerCase()==='img')throw Error('HTMLImageElement dependency');
            return create(tag);
        };
        for(const section of document.querySelectorAll('.case')) {
            const p=section.querySelector('p'),handler=new Layout();
            handler.update(p,p.innerHTML);handlers.push(handler);
        }
        document.createElement=create;
        window.testImagesReady=Promise.all([...document.querySelectorAll('[data-crml-tutorial-media]')].map(media=>{
            const align=Number(media.closest('.case').dataset.alignment);
            const w=align===0?258:400,h=align===0?80:align===1?800:80;
            media.dataset.testWidth=w;media.dataset.testHeight=h;
            const url='data:image/svg+xml,'+encodeURIComponent(`<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}"><rect width="100%" height="100%" fill="#53a6bd"/><circle cx="${w/2}" cy="${h/2}" r="${Math.min(w,h)/3}" fill="#fff"/></svg>`);
            media.style.backgroundImage='url("'+url+'")';
            // Test resource readiness only; production never uses this probe.
            return new Promise((resolve,reject)=>{const probe=new Image();probe.onload=resolve;probe.onerror=reject;probe.src=url;});
        }));
    })()''')
    await browser.evaluate('window.testImagesReady.then(()=>true)')
    for width, height in ((1280, 900), (1920, 1080)):
        await browser.call('Emulation.setDeviceMetricsOverride', width=width, height=height,
                           deviceScaleFactor=1, mobile=False)
        problems = await browser.evaluate('''(() => {
          const failures=[];
          for(const section of document.querySelectorAll('.case')) {
            const below=Number(section.dataset.position),align=Number(section.dataset.alignment);
            const p=section.querySelector('[data-crml-tutorial-layout="1"]'),media=p.querySelector('[data-crml-tutorial-media]');
            if(media.closest('[cohinline]'))failures.push('image inside inline renderer');
            const row=media.parentElement,text=p.children[below?0:1];
            const a=row.getBoundingClientRect(),b=text.getBoundingClientRect();
            const i=media.getBoundingClientRect(),r=p.getBoundingClientRect();
            const gap=below?a.top-b.bottom:b.top-a.bottom;
            if(Math.abs(gap-innerHeight*0.02)>1)failures.push('missing gap');
            if(Math.abs(i.width-r.width*0.60)>1 || Math.abs(i.height-innerHeight*0.12)>1)failures.push('frame bounds');
            const expected=align===1?r.left:align===2?r.right-i.width:r.left+(r.width-i.width)/2;
            if(Math.abs(i.left-expected)>1)failures.push('alignment '+align);
            if(!text.textContent.includes('<plain text>') || text.querySelector('plain'))failures.push('text escaping');
          }
          return failures;
        })()''')
        if problems:
            raise AssertionError(f'Tutorial layout at {width}x{height}: {problems}')
        shot=await browser.call('Page.captureScreenshot',format='png')
        (artifact_dir/f'tutorial-layout-{width}.png').write_bytes(base64.b64decode(shot['data']))
        # Check actual painted texture bounds; a correctly sized empty box or
        # a stretched texture must not pass just because its CSS looks right.
        pixel_check='''(async function(url) {
            const capture=new Image();await new Promise((ok,fail)=>{capture.onload=ok;capture.onerror=fail;capture.src=url;});
            const canvas=document.createElement('canvas');canvas.width=capture.width;canvas.height=capture.height;
            const ctx=canvas.getContext('2d');ctx.drawImage(capture,0,0);
            const data=ctx.getImageData(0,0,canvas.width,canvas.height).data,failures=[];
            for(const media of document.querySelectorAll('[data-crml-tutorial-media]')) {
                const r=media.getBoundingClientRect(),section=media.closest('.case');
                const align=Number(section.dataset.alignment),below=Number(section.dataset.position);
                const scale=Math.min(r.width/Number(media.dataset.testWidth),r.height/Number(media.dataset.testHeight));
                const w=Number(media.dataset.testWidth)*scale,h=Number(media.dataset.testHeight)*scale;
                const left=r.left+(r.width-w)*(align===1?0:align===2?1:.5),top=below?r.top:r.bottom-h;
                let minX=Infinity,minY=Infinity,maxX=-1,maxY=-1;
                for(let y=Math.max(0,Math.floor(r.top));y<Math.min(canvas.height,Math.ceil(r.bottom));++y)
                    for(let x=Math.max(0,Math.floor(r.left));x<Math.min(canvas.width,Math.ceil(r.right));++x) {
                        const i=(y*canvas.width+x)*4;
                        if(Math.abs(data[i]-83)<3&&Math.abs(data[i+1]-166)<3&&Math.abs(data[i+2]-189)<3) {
                            minX=Math.min(minX,x);maxX=Math.max(maxX,x);minY=Math.min(minY,y);maxY=Math.max(maxY,y);
                        }
                    }
                if(maxX<0 || Math.abs(minX-left)>2 || Math.abs(minY-top)>2 || Math.abs(maxX-minX+1-w)>2 || Math.abs(maxY-minY+1-h)>2)
                    failures.push('missing or distorted painted image '+below+'/'+align);
            }
            return failures;
        })'''
        problems=await browser.evaluate(pixel_check+'('+json.dumps('data:image/png;base64,'+shot['data'])+')')
        if problems:
            raise AssertionError(f'Tutorial rendered pixels at {width}x{height}: {problems}')
    problems=await browser.evaluate('''(() => {
        const failures=[],h=handlers[1],p=h.element;
        const observer=new MutationObserver(()=>{});observer.observe(h.node,{childList:true,subtree:true,attributes:true});
        for(let i=0;i<100;++i)h.update(p,h.value);
        if(observer.takeRecords().length)failures.push('unchanged model rebuilt nodes');observer.disconnect();
        const panel=p.parentElement;panel.style.display='none';panel.style.display='';h.update(p,h.value);
        if(h.node.querySelector('[data-crml-tutorial-media]').getBoundingClientRect().height<=0)failures.push('hidden-to-visible image');
        const stock='Original <b>game rich text</b>';p.innerHTML=stock;h.update(p,stock);
        if(p.innerHTML!==stock || p.style.display==='none' || h.node)failures.push('stock tutorial changed');
        const bad='<span data-crml-tutorial-image="https://remote/image.png" data-crml-tutorial-layout="0,0,60,12,2">text</span>';
        h.update(p,bad);if(h.node)failures.push('remote image accepted');
        const section=document.querySelector('.case');
        section.querySelector('[data-crml-tutorial-media]').style.backgroundImage='url("data:image/png;base64,AA==")';
        if(!section.textContent.includes('Separate text block') || section.querySelector('button').getBoundingClientRect().height<=0)failures.push('missing image hid text/control');
        window.dispatchEvent(new Event('pagehide'));
        if(document.querySelector('[data-crml-tutorial-layout="1"]') || __crmlTutorialLayout.status().owners)failures.push('page cleanup');
        if([...document.querySelectorAll('.case > p')].some(p=>p.style.display==='none'))failures.push('original text not restored');
        return failures;
    })()''')
    if problems:
        raise AssertionError(f'Tutorial lifecycle: {problems}')
    await browser.call('Page.navigate', url=fixture.resolve().as_uri())
    await browser.wait('document.querySelectorAll(".case").length === 6')
    await browser.evaluate('window.engine={}')
    await browser.evaluate(script)
    if not await browser.evaluate('''!__crmlTutorialLayout.status().registered && !document.querySelector('[data-crml-tutorial-media]') &&
        [...document.querySelectorAll('.case > p')].every(p=>p.textContent.includes('Separate text block') && p.getBoundingClientRect().height>0)'''):
        raise AssertionError('Missing custom binding broke readable text fallback')
    print('PASS: tutorial painted image proportions, placement, bounds, spacing, no image-element dependency, lifecycle and fallback')


async def run(args):
    try:
        from websockets.asyncio.client import connect
    except ImportError as error:
        raise SystemExit('Install browser test dependencies: python -m pip install -r requirements-tests.txt') from error
    browser_path = args.browser or default_browser()
    if not browser_path or not browser_path.is_file():
        raise SystemExit('Provide an installed Edge/Chromium executable using --browser PATH')
    artifact_dir = ROOT / '.local/native-ui-page-tests' / time.strftime('%Y%m%d-%H%M%S')
    artifact_dir.mkdir(parents=True, exist_ok=False)
    payload = args.payload.read_text(encoding='utf-8').replace('__CRML_SETTINGS_PAGE__', '1')
    shell = SHELL
    if args.css:
        # Optional locally extracted stylesheet stays in the ignored artifact.
        shell = shell.replace('</style>', '</style><style>' + args.css.read_text(encoding='utf-8') + '</style>', 1)
    (artifact_dir / 'fixture.html').write_text(shell.replace('PAYLOAD', payload), encoding='utf-8')
    settings_shell=(ROOT/'tests/fixtures/settings-ui.html').read_text(encoding='utf-8')
    (artifact_dir/'settings-fixture.html').write_text(settings_shell.replace('__CRML_PANEL__',payload),encoding='utf-8')
    drawing_payload=(ROOT/'runtime/diagnostics/native_ui_drawing.html').read_text(encoding='utf-8').replace('__CRML_DRAWING_PAGE__','1')
    drawing_shell=(ROOT/'tests/fixtures/drawing-ui.html').read_text(encoding='utf-8')
    if args.css:
        drawing_shell=drawing_shell.replace('</style>', '</style><style>'+args.css.read_text(encoding='utf-8')+'</style>',1)
    (artifact_dir/'drawing-fixture.html').write_text(drawing_shell.replace('__CRML_DRAWING__',drawing_payload),encoding='utf-8')
    feedback_payload=(ROOT/'runtime/diagnostics/native_ui_feedback.html').read_text(encoding='utf-8').replace('__CRML_FEEDBACK_PAGE__','1')
    feedback_shell=(ROOT/'tests/fixtures/feedback-ui.html').read_text(encoding='utf-8')
    (artifact_dir/'feedback-fixture.html').write_text(feedback_shell.replace('__CRML_FEEDBACK__',feedback_payload),encoding='utf-8')
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
            if args.tutorial_only:
                if not args.tutorial_layout:
                    raise ValueError("--tutorial-only requires --tutorial-layout")
                browser = Browser(connection)
                await browser.call("Page.enable")
            else:
                browser = await check(connection, artifact_dir)
            if args.tutorial_layout:
                await check_tutorial_layout(browser, args.tutorial_layout, artifact_dir)
            if not args.tutorial_only:
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
    parser.add_argument('--tutorial-only', action='store_true', help='Run only the tutorial layout and lifecycle checks')
    parser.add_argument('--tutorial-layout', type=Path, help='Fixture emitted by crml_tutorial_service_tests --write-layout')
    parser.add_argument('--css', type=Path, help='Optional local stylesheet; never copied into public fixtures')
    parser.add_argument('--payload', type=Path,
                        default=ROOT / 'runtime/diagnostics/native_ui_panel.html',
                        help='Authored payload to test, for reproducing an older regression')
    asyncio.run(run(parser.parse_args()))
