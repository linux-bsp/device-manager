// Optional real-browser smoke test. Use a disposable OTA feed: this publishes a version.
// CHROMIUM_BIN=/path/to/chrome node tests/browser-smoke.mjs URL BUNDLE OUTPUT_DIR
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, readFile, writeFile, mkdir, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';

const [inputUrl, bundle, outputDir] = process.argv.slice(2);
const url=(inputUrl||'').replace(/\/+$/,'');
assert(url && bundle && outputDir && process.env.CHROMIUM_BIN,
    'Provide URL, BUNDLE, OUTPUT_DIR and CHROMIUM_BIN');
await mkdir(outputDir, {recursive: true});
const profile = await mkdtemp(join(tmpdir(), 'ota-browser-'));
const browser = spawn(process.env.CHROMIUM_BIN, [
    '--headless', '--no-sandbox', '--disable-dev-shm-usage', '--no-proxy-server',
    '--no-first-run', '--no-default-browser-check', '--remote-debugging-port=0',
    `--user-data-dir=${profile}`, 'about:blank',
], {stdio: ['ignore', 'ignore', 'ignore']});
let ws;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(fn, label, timeout=60000) {
    const start = Date.now();
    while (Date.now() - start < timeout) {
        const result = await fn();
        if (result) return result;
        await pause(100);
    }
    throw new Error(`Timed out: ${label}`);
}
try {
    const port = await until(async()=>{
        try { return (await readFile(join(profile,'DevToolsActivePort'),'utf8')).split('\n')[0]; }
        catch { assert(browser.exitCode === null, 'Browser exited'); return null; }
    }, 'DevTools startup');
    const target = await (await fetch(`http://127.0.0.1:${port}/json/new?about:blank`, {method:'PUT'})).json();
    ws = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
    let sequence=0;
    const pending = new Map();
    const errors = [];
    ws.onmessage = event=>{
        const message=JSON.parse(event.data);
        if(message.id){
            const request=pending.get(message.id);if(!request)return;
            pending.delete(message.id);clearTimeout(request.timer);
            if(message.error)request.reject(new Error(JSON.stringify(message.error)));
            else request.resolve(message.result);
        } else if(message.method==='Runtime.exceptionThrown') errors.push(message.params.exceptionDetails);
    };
    function rpc(method,params={}){
        return new Promise((resolve,reject)=>{
            const id=++sequence;
            const timer=setTimeout(()=>{pending.delete(id);reject(new Error(`CDP timeout: ${method}`));},30000);
            pending.set(id,{resolve,reject,timer});ws.send(JSON.stringify({id,method,params}));
        });
    }
    async function evaluate(expression){
        const result=await rpc('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});
        if(result.exceptionDetails)throw new Error(JSON.stringify(result.exceptionDetails));
        return result.result.value;
    }
    await rpc('Runtime.enable');await rpc('Page.enable');
    await rpc('Emulation.setDeviceMetricsOverride',{width:1280,height:1000,deviceScaleFactor:1,mobile:false});
    const seeded=await fetch(url+'/api/devices/browser-test',{method:'POST',headers:{'X-OTA-Request':'1'},
        body:'BSPOTASTATE1 browser-test ti-am62x am62x-sd-ab-v1 1 65537 test-boot success 1 65537\n'});
    assert.equal(seeded.status,200,'Device report should be persisted');
    for (const phase of ['installing','install-failed']) {
        const r=await fetch(url+'/api/devices/browser-test',{method:'POST',headers:{'X-OTA-Request':'1'},
            body:`BSPOTASTATE1 browser-test ti-am62x am62x-sd-ab-v1 2 65538 next-boot ${phase} 0 65537\n`});
        assert.equal(r.status,200);
    }
    await rpc('Page.navigate',{url});
    await until(()=>evaluate("document.querySelector('#current-version')?.textContent !== '加载中…' && !!document.querySelector('#firmware')"),'page state');
    await until(()=>evaluate("document.querySelector('#devices')?.textContent.includes('browser-test') && document.querySelector('#devices').textContent.includes('升级成功')"),'device result table');
    assert.equal(await evaluate("document.querySelectorAll('#devices tr').length"),2);
    await evaluate("document.querySelector('#devices details').open=true");
    assert(await evaluate("document.querySelector('#devices details').textContent.includes('正在安装') && document.querySelector('#devices details').textContent.includes('安装失败')"));
    const before=await (await fetch(url+'/api/state')).json();
    const doc=await rpc('DOM.getDocument');
    const input=await rpc('DOM.querySelector',{nodeId:doc.root.nodeId,selector:'#firmware'});
    await rpc('DOM.setFileInputFiles',{nodeId:input.nodeId,files:[resolve(bundle)]});
    await until(()=>evaluate("!document.querySelector('#upload-button').disabled"),'file selection');
    await evaluate("document.querySelector('#upload-button').click()");
    await until(()=>evaluate("document.querySelector('#upload-status').textContent.includes('已保存为待发布')"),'upload and validation');
    const uploaded=await (await fetch(url+'/api/state')).json();
    assert.equal(uploaded.latest,before.latest,'Upload must not publish');
    const draft=uploaded.drafts.find(d=>!before.drafts.some(old=>old.id===d.id));
    assert(draft && !draft.publication,'New draft should be unpublished');
    assert(draft.tag && draft.firmware_id);
    const expectedTag=draft.tag;
    async function screenshot(name){
        const {data}=await rpc('Page.captureScreenshot',{format:'png',captureBeyondViewport:true});
        await writeFile(join(outputDir,name),Buffer.from(data,'base64'));
    }
    await screenshot('uploaded-desktop.png');
    await evaluate(`document.querySelector('[data-publish="${draft.id}"]').click()`);
    await until(()=>evaluate("document.querySelector('#notice').textContent.startsWith('已发布 #')"),'publish button');
    await until(async()=>{
        const state=await (await fetch(url+'/api/state')).json();
        return state.latest>before.latest;
    },'latest publication');
    await until(()=>evaluate(`document.querySelector('#current-version').textContent===${JSON.stringify(expectedTag)} && !document.querySelector('[data-publish]')`),'published UI');
    const after=await (await fetch(url+'/api/state')).json();
    assert(await evaluate("document.querySelector('.upload-main').getBoundingClientRect().right < document.querySelector('.upload-sidebar').getBoundingClientRect().left"),'Upload sidebar');
    assert(await evaluate("document.querySelector('#drafts-table').closest('.card').getBoundingClientRect().width > document.querySelector('#panel-manage').getBoundingClientRect().width-2"),'Full width publishing');
    assert(await evaluate("!document.querySelector('#panel-manage').textContent.includes('01 /') && !document.querySelector('#panel-manage').textContent.includes('02 /')"));
    for(const tab of ['history','releases','audit','manage']){
        await evaluate(`document.querySelector('#tab-${tab}').click()`);
        assert(await evaluate(`!document.querySelector('#panel-${tab}').hidden && document.querySelectorAll('[role=tabpanel]:not([hidden])').length===1`),'Tab '+tab);
    }
    // Tab URL survives full page reload; refresh/filter never resets selection.
    await evaluate("document.querySelector('#tab-history').click()");
    await rpc('Page.reload');
    await until(()=>evaluate("document.querySelectorAll('#devices tr').length===2"),'reload history');
    assert(await evaluate("location.hash==='#history' && !document.querySelector('#panel-history').hidden"));
    await evaluate("document.querySelector('[data-refresh=history]').click()");
    await until(()=>evaluate("!document.querySelector('[data-refresh=history]').disabled"),'tab refresh');
    assert(await evaluate("!document.querySelector('#panel-history').hidden"));
    async function search(tab,text){
        await evaluate(`document.querySelector('#tab-${tab}').click();document.querySelector('#search-${tab}').value=${JSON.stringify(text)};document.querySelector('#search-${tab}').dispatchEvent(new Event('input'))`);
    }
    await search('history',draft.firmware_id.slice(32));
    assert.equal(await evaluate("document.querySelectorAll('#devices tr:not([hidden])').length"),1,'Search full hash beyond display prefix');
    await search('history','browser-test 升级成功');
    assert.equal(await evaluate("document.querySelectorAll('#devices tr:not([hidden])').length"),1);
    await search('history','no-such-device');
    assert(await evaluate("!document.querySelector('#empty-history').hidden"));
    await search('history','');
    for(const tab of ['manage','releases']){
        await search(tab,draft.tag+' '+draft.firmware_id.slice(0,8));
        assert.equal(await evaluate(`document.querySelectorAll('#${tab==='manage'?'drafts':'releases'} tr:not([hidden])').length`),1);
        await rpc('Page.reload');
        await until(()=>evaluate(`document.querySelector('#search-${tab}')?.value===${JSON.stringify(draft.tag+' '+draft.firmware_id.slice(0,8))} && document.querySelector('#count-${tab}').textContent==='1 / 1 条'`),'filter survives reload');
        assert(await evaluate(`!document.querySelector('#panel-${tab}').hidden`));
        await search(tab,'');
    }
    assert.deepEqual(await evaluate("[...document.querySelectorAll('#drafts-table th')].map(x=>x.textContent)"),['产品','固件版本','固件ID','大小','操作','详情']);
    await evaluate("document.querySelector('#tab-manage').click()");
    assert(await evaluate("!document.querySelector('#overview a.download')"),'Download only in details');
    for(const table of ['drafts','releases'])assert(await evaluate(`document.querySelector('#${table} .publishing').disabled && document.querySelector('#${table} .publishing').textContent==='当前版本使用中' && getComputedStyle(document.querySelector('#${table} .publishing')).backgroundColor==='rgb(229, 231, 235)'`),'Grey current release button');
    await screenshot('published-desktop.png');
    await rpc('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:1,mobile:true});
    await pause(150);
    assert(await evaluate('document.documentElement.scrollWidth<=window.innerWidth+1'),'Mobile page overflows');
    await screenshot('published-mobile.png');
    await rpc('Page.navigate',{url:url+'/firmware?kind=release&id='+after.latest+'&tab=releases'});
    await until(()=>evaluate("document.querySelector('#firmware-detail')?.textContent.includes('设备下载次数')"),'firmware detail');
    const detail=await evaluate("document.querySelector('#firmware-detail').textContent");
    for(const field of ['上传时间','版本号','更新成功次数','更新失败次数','是否有效',expectedTag,draft.firmware_id]) assert(detail.includes(field),field);
    assert(await evaluate("document.querySelector('#overview').hidden"));
    assert(await evaluate("document.querySelector('#firmware-detail a.download').href.includes('/api/download/release/')"));
    assert(await evaluate('document.documentElement.scrollWidth<=window.innerWidth+1'),'Detail page overflows');
    await screenshot('firmware-detail-mobile.png');
    await rpc('Emulation.setDeviceMetricsOverride',{width:1280,height:1000,deviceScaleFactor:1,mobile:false});
    await screenshot('firmware-detail-desktop.png');
    assert(await evaluate("document.querySelector('#firmware-detail a').getAttribute('href')==='/#releases'"),'Return to original tab');
    // Use real HTTP headers, persist context and show it in upgrade history.
    const contextHeaders={'X-OTA-Request':'1','X-OTA-Device':'network-audit','X-OTA-IP':'192.0.2.99','X-OTA-MAC':'02:00:00:00:00:99','X-OTA-Hostname':'am625-audit','X-OTA-Boot':'network-boot'};
    const failure=await fetch(url+'/api/devices/network-audit',{method:'POST',headers:contextHeaders,body:`BSPOTASTATE2 network-audit ti-am62x am62x-sd-ab-v1 ${after.latest} 1 network-boot install-failed 1 1 simulated-failure\n`});
    assert.equal(failure.status,200);
    await rpc('Page.navigate',{url:url+'/#manage'});
    await until(()=>evaluate("!!document.querySelector('#drafts .invalid')"),'quarantined button');
    assert(await evaluate("document.querySelector('#drafts .invalid').disabled && getComputedStyle(document.querySelector('#drafts .invalid')).backgroundColor==='rgb(180, 35, 24)'"));
    await screenshot('invalid-firmware.png');
    for(const term of ['192.0.2.99','02:00:00:00:00:99','am625-audit']){
        await search('history',term);
        assert.equal(await evaluate("document.querySelectorAll('#devices tr:not([hidden])').length"),1,'Network filter '+term);
    }
    await evaluate("document.querySelector('#devices tr:not([hidden]) details').open=true");
    await screenshot('network-audit.png');
    await search('audit',draft.firmware_id.slice(32));
    assert(await evaluate("document.querySelectorAll('#audit tr:not([hidden])').length>0"));
    await evaluate("document.querySelector('#tab-history').click();document.querySelector('#tab-releases').click();history.back()");
    await until(()=>evaluate("!document.querySelector('#panel-history').hidden"),'browser back restores tab');
    await rpc('Page.navigate',{url:url+'/#sol'});
    await until(()=>evaluate("!document.querySelector('#sol-panel').hidden"),'SOL sidebar route');
    assert(await evaluate("document.querySelector('#sol-panel').textContent.includes('只读实时日志')"));
    await rpc('Page.reload',{});
    await until(()=>evaluate("document.querySelector('#sol-panel')&&!document.querySelector('#sol-panel').hidden"),'SOL refresh retains module');
    assert(await evaluate("document.querySelector('#overview').hidden"));
    if(process.env.SOL_VIEW_TOKEN){
        if(process.env.SOL_OPEN_VIEW)await until(()=>evaluate("document.querySelector('#sol-auth').hidden"),'open SOL view hides credential form');
        else await evaluate(`document.querySelector('#sol-key').value=${JSON.stringify(process.env.SOL_VIEW_TOKEN)};document.querySelector('#sol-connect').click()`);
        await until(()=>evaluate("document.querySelector('#sol-output').textContent.includes('SOL boot fixture')"),'authenticated SOL log replay');
        assert(await evaluate("document.querySelector('#sol-meta').textContent.includes('storage=persistent')"),'SOL storage metadata');
        assert(await evaluate("!document.querySelector('#sol-output script')"),'SOL log text cannot execute HTML');
        await evaluate("document.querySelector('#sol-filter').value='no-match';document.querySelector('#sol-filter').dispatchEvent(new Event('input'))");
        assert.equal(await evaluate("document.querySelector('#sol-output').textContent"),'');
        await evaluate("document.querySelector('#sol-filter').value='';document.querySelector('#sol-filter').dispatchEvent(new Event('input'))");
        await rpc('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:1,mobile:true});
        assert(await evaluate('document.documentElement.scrollWidth<=window.innerWidth+1'),'SOL mobile page overflows');
        await screenshot('sol-mobile.png');
        await rpc('Emulation.setDeviceMetricsOverride',{width:1280,height:1000,deviceScaleFactor:1,mobile:false});
    }
    await screenshot('sol-sidebar.png');
    await evaluate("document.querySelector('#module-ota').click()");
    await until(()=>evaluate("!document.querySelector('#overview').hidden"),'return to OTA');
    assert.equal(errors.length,0,JSON.stringify(errors));
    await writeFile(join(outputDir,'browser-result.json'),JSON.stringify({
        uploadCreatedDraft:true,uploadDidNotPublish:true,deviceResultsRendered:true,publication:after.latest,
        firmwareVersion:expectedTag,detailsVerified:true,tabsVerified:true,downloadLinksVerified:true,mobileNoOverflow:true,javascriptErrors:errors,
    },null,2)+'\n');
    console.log('PASS: Chromium file upload -> unpublished draft -> publish; desktop/mobile UI and JS verified');
} finally {
    if(ws)ws.close();
    browser.kill('SIGTERM');
    await new Promise(resolve=>{if(browser.exitCode!==null)resolve();else browser.once('exit',resolve);});
    await rm(profile,{recursive:true,force:true,maxRetries:5,retryDelay:100});
}
