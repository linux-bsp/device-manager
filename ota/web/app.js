'use strict';
const $ = id => document.getElementById(id);
let busy = false;
let refreshing = null;
function notice(message, error=false){$('notice').textContent=message;$('notice').className='notice'+(error?' error':'');$('notice').hidden=false;}
function size(bytes){return (bytes/1048576).toFixed(1)+' MiB';}
function cell(row,text){const td=document.createElement('td');td.textContent=text;row.append(td);return td;}
function detailLink(item,tab=activeTab()){const a=document.createElement('a');a.textContent='查看详情';a.href='/firmware?kind='+((item.id||'').startsWith('draft-')?'draft':'release')+'&id='+encodeURIComponent(item.id)+'&tab='+tab;return a;}
function identityCells(row,item){cell(row,item.product||'未知');cell(row,item.tag||item.firmware_version||'未知');const td=cell(row,item.firmware_id?item.firmware_id.slice(0,8):'未知');td.title=item.firmware_id||'历史记录未保存固件 ID';}
function invalidButton(){const b=document.createElement('button');b.type='button';b.className='invalid';b.disabled=true;b.textContent='无效固件，禁止发布';b.title='固件已隔离，不能发布到设备';return b;}
function publishingButton(){const b=document.createElement('button');b.type='button';b.className='publishing';b.disabled=true;b.textContent='当前发布版本';return b;}
function activeTab(){return document.querySelector('[data-tab][aria-selected=true]')?.dataset.tab||'manage';}
function searchable(row,item){row.dataset.search=JSON.stringify(item);}
const lists={manage:'drafts',history:'devices',releases:'releases',audit:'audit'};
function filterRows(tab){const tokens=$('search-'+tab).value.trim().toLowerCase().split(/\s+/).filter(Boolean);let visible=0,total=0;for(const row of $(lists[tab]).children){total++;const text=((row.dataset.search||'')+' '+row.textContent).toLowerCase();row.hidden=!tokens.every(t=>text.includes(t));if(!row.hidden)visible++;}$('count-'+tab).textContent=`${visible} / ${total} 条`;$('empty-'+tab).hidden=visible>0;}
function downloadLink(item){const link=document.createElement('a');link.className='download';link.textContent='下载固件包';link.href='/api/download/'+(item.id.startsWith('draft-')?'draft':'release')+'/'+encodeURIComponent(item.id);return link;}
function badge(td,text,current=false){const span=document.createElement('span');span.className='badge'+(current?' green':'');span.textContent=text;td.append(span);}
function controls(){ const disabled=busy||!!refreshing; $('upload-button').disabled=disabled||!$('firmware').files.length; $('firmware').disabled=disabled; document.querySelectorAll('[data-refresh]').forEach(b=>b.disabled=disabled); document.querySelectorAll('[data-publish]').forEach(b=>b.disabled=disabled); }
function state(){
 if(refreshing)return refreshing;
 refreshing=loadState().finally(()=>{refreshing=null;controls();});
 controls();return refreshing;
}
async function loadState(){
 const response=await fetch('/api/state',{cache:'no-store'});if(!response.ok)throw new Error('无法读取版本列表，请检查服务日志。');
 const data=await response.json();
 renderFleet(data);
 $('service-health').textContent='管理服务已连接';$('service-health').classList.remove('error');
 if(location.pathname==='/firmware'){renderDetails(data);return;}
 // Resolve the active publication from the release list and include it in the
 // publishing table beside unpublished drafts.
 const phases={'requested':'请求升级','installing':'正在安装','rebooting':'等待重启','boot-testing':'启动健康检查中','success':'升级成功','install-failed':'安装失败','rolled-back':'已回滚','interrupted':'升级中断','reboot-failed':'重启失败','up-to-date':'无需升级','device-error':'设备异常','verify-failed':'固件校验失败'};
 const why=r=>r==='not-reported'?'原因未上报':r==='none'?'—':r||'未上报';
 const fmt=v=>v?`v${v>>>16}.${(v>>>8)&255}.${v&255}`:'未知';
 const when=v=>new Date(v*1000).toLocaleString();
 const identity=d=>data.releases.find(r=>r.publication===d.publication)||{product:d.product,tag:fmt(d.version)};
 for(const [tab,list] of Object.entries(lists))$(list).replaceChildren();
 $('drafts-empty').hidden=true;$('releases-empty').hidden=true;
 const candidates=new Map();
 const firmwareKey=item=>item.firmware_id?JSON.stringify([item.product,item.layout,item.tag||item.firmware_version,item.firmware_id]):item.id;
 for(const draft of [...data.drafts].sort((a,b)=>b.created-a.created)){
  const key=firmwareKey(draft);
  if(!candidates.has(key))candidates.set(key,draft);
 }
 // The published snapshot replaces its uploaded draft in this table.
 for(const release of data.releases.filter(item=>item.published))candidates.set(firmwareKey(release),release);
 const manageItems=[...candidates.values()];
 for(const [items,tbody] of [[manageItems,'drafts'],[data.releases,'releases']]){
  for(const d of [...items].sort((a,b)=>tbody==='drafts'?(Number(!!b.published)-Number(!!a.published))||b.created-a.created||b.publication-a.publication:b.created-a.created||b.publication-a.publication)){
   const row=document.createElement('tr');if(tbody==='drafts')row.dataset.draft=d.id;
   identityCells(row,d);cell(row,size(d.bytes));
   const action=cell(row,'');
   if(!d.valid)action.append(invalidButton());
   else if(d.published)action.append(publishingButton());
   else {const draft=tbody==='drafts'?d:data.drafts.find(x=>x.valid&&x.firmware_id===d.firmware_id);if(draft){const b=document.createElement('button');b.type='button';b.textContent='发布到设备';b.dataset.publish=draft.id;b.onclick=()=>publish(draft.id);action.append(b);}else action.textContent='未发布';}
   cell(row,'').append(detailLink(d,tbody==='drafts'?'manage':'releases'));
   searchable(row,{...d,status:!d.valid?'无效':d.published?'已发布':'未发布',created_text:when(d.created)});$(tbody).append(row);
  }
 }
 for(const d of [...(data.device_history||[])].sort((a,b)=>b.updated-a.updated||b.publication-a.publication)){
  const item=identity(d),row=document.createElement('tr');identityCells(row,item);cell(row,d.device);
  const result=cell(row,phases[d.phase]||d.phase);const reason=document.createElement('small');reason.textContent=why(d.reason);result.append(reason);
  cell(row,when(d.updated));const td=cell(row,'');td.className='network-info';
  const details=document.createElement('details'),summary=document.createElement('summary');summary.textContent=`查看过程与设备信息（${d.events.length} 次上报）`;details.append(summary);
  const line=text=>{const e=document.createElement('div');e.textContent=text;details.append(e);};
  if(d.imported)line('旧数据迁移：迁移前仅保留当时的最后状态。');
  for(const e of d.events)line(`${when(e.reported_at)} · ${phases[e.phase]||e.phase} · 运行 ${fmt(e.running)} · ${why(e.reason)}`);
  line('升级来源启动 ID：'+d.source_boot);
  if(!(d.network||[]).length)line('IP / MAC / hostname：历史设备未上报');
  for(const n of d.network||[])line(`${when(n.first_seen)} — ${when(n.last_seen)} · ${phases[n.phase]||n.phase} · IP ${n.ip} · MAC ${n.mac} · hostname ${n.hostname} · 来源 IP ${n.peer_ip} · 请求 ${n.requests} 次 · 启动 ID ${n.request_boot}`);
  td.append(details);if(item.id)td.append(detailLink(item,'history'));searchable(row,{...d,firmware:item});$('devices').append(row);
 }
 const actions={'manual-fallback-published':'人工应急回退发布','quarantined':'标记固件异常','distribution-stopped':'停止全部下发','fallback-selected':'选定正常版本','fallback-published':'已回退发布','no-confirmed-fallback':'无确认正常版本，保持停发'};
 for(const e of [...(data.audit_events||[])].sort((a,b)=>b.timestamp-a.timestamp)){
  const item=identity(e),row=document.createElement('tr');identityCells(row,item);cell(row,when(e.timestamp));cell(row,e.device);cell(row,`${e.category==='device-error'?'设备异常':'升级失败'} / ${why(e.reason)}`);
  const td=cell(row,actions[e.action]||e.action);td.className='network-info';const details=document.createElement('details');const title=document.createElement('summary');title.textContent='查看溯源';details.append(title);
  for(const text of [`发布记录 ${e.publication} · 回退来源 ${e.source_publication||'无'} · 回退发布 ${e.replacement_publication||'无'}`,e.kernel_sha256,e.rootfs_sha256,e.boot]){const line=document.createElement('div');line.textContent=text;details.append(line);}td.append(details);if(item.id)td.append(detailLink(item,'audit'));searchable(row,{...e,firmware:item});$('audit').append(row);
 }
 for(const tab of Object.keys(lists))filterRows(tab);
 controls();
}

function renderDetails(data){
 $('overview').hidden=true;
 const pane=$('firmware-detail');pane.hidden=false;pane.replaceChildren();
 const back=document.createElement('a');back.href='/#'+(new URLSearchParams(location.search).get('tab')||'manage');back.textContent='← 返回固件列表';pane.append(back);
 const refresh=document.createElement('button');refresh.className='secondary';refresh.textContent='刷新详情';refresh.style.marginLeft='20px';refresh.onclick=()=>state().catch(e=>notice(e.message,true));pane.append(refresh);
 const params=new URLSearchParams(location.search);
 const collection=params.get('kind')==='draft'?data.drafts:data.releases;
 const item=collection.find(x=>x.id===params.get('id'));
 const title=document.createElement('h1');title.textContent='固件详情';pane.append(title);
 if(!item){const message=document.createElement('p');message.textContent='未找到该固件。';pane.append(message);return;}
 document.title=(item.tag||item.firmware_version)+' · 固件详情';
 pane.append(downloadLink(item));
 const publications=data.releases.filter(x=>x.firmware_id===item.firmware_id);
 const ids=new Set(publications.map(x=>x.publication));
 const history=(data.device_history||[]).filter(x=>ids.has(x.publication));
 const downloads=(data.download_stats||[]).filter(x=>ids.has(x.publication));
 const sum=key=>downloads.reduce((n,x)=>n+(x[key]||0),0);
 const success=history.filter(x=>x.phase==='success').length;
 const failures=history.filter(x=>x.category==='upgrade-failure');
 const incidents=history.filter(x=>x.category==='device-error');
 const active=publications.find(x=>x.published);
 const table=document.createElement('table');pane.append(table);
 const rows=[['上传时间',item.uploaded_at?new Date(item.uploaded_at*1000).toLocaleString():'未知'],
 ['版本号',item.tag||item.firmware_version],['内容哈希',item.firmware_id],['产品 / 布局',item.product+' / '+item.layout],
 ['固件大小',size(item.bytes)],['发布状态',active?'已发布 #'+active.publication:'未发布'],
 ['是否有效',item.valid?'有效（上传校验通过，未被隔离）':'无效（已隔离，禁止发布）'],
 ['设备下载次数',String(sum('identified_downloads'))],['未识别设备的下载次数',String(sum('downloads')-sum('identified_downloads'))],
 ['更新成功次数',String(success)],['更新失败次数',String(failures.length)],['设备异常次数',String(incidents.length)],
 ['历史发布编号',publications.map(x=>'#'+x.publication).join('、')||'尚未发布']];
 for(const [key,value] of rows){const row=document.createElement('tr');cell(row,key);const td=cell(row,value);td.style.whiteSpace='normal';td.style.overflowWrap='anywhere';table.append(row);}
 const note=document.createElement('p');note.className='muted';note.textContent='统计按相同内容哈希汇总。下载按 rootfs.itb 完整响应计次，重试和恢复下载会再次计数；不代表安装成功。未携带设备 ID 的请求单列，旧版本启用统计前的下载无法追溯。升级结果按每次升级事务最终状态计数，成功必须经过重启健康确认。';pane.append(note);
 const heading=document.createElement('h2');heading.textContent='失败与设备异常原因';pane.append(heading);
 const errors=failures.concat(incidents).sort((a,b)=>b.updated-a.updated);
 for(const entry of errors){const line=document.createElement('p');line.textContent=`#${entry.publication} · ${entry.device} · ${entry.phase} · ${entry.reason||'原因未上报'} · ${new Date(entry.updated*1000).toLocaleString()}`;pane.append(line);}
 if(!errors.length){const line=document.createElement('p');line.textContent='暂无失败或设备异常记录。';pane.append(line);}
 const candidate=data.drafts.find(x=>x.firmware_id===item.firmware_id&&x.valid);
 if(!item.valid)pane.append(invalidButton());
 else if(active)pane.append(publishingButton());
 if(!active&&candidate){const button=document.createElement('button');button.dataset.publish=candidate.id;button.textContent='发布到设备';button.onclick=()=>publish(candidate.id);pane.append(button);}
 controls();
}
async function publish(id){
 busy=true;controls();
 try{const r=await fetch('/api/publish/'+id,{method:'POST',headers:{'X-OTA-Request':'1'}});const result=await r.json();if(!r.ok)throw new Error(result.error||'发布失败');notice(`已发布 #${result.publication}。设备将在下一次检查时下载该版本。`);await state();}
 catch(error){notice(error.message,true);}finally{busy=false;controls();}
}
$('firmware').addEventListener('change',()=>{const f=$('firmware').files[0];$('file-info').textContent=f?f.name+' · '+size(f.size):'';controls();});
$('upload-form').addEventListener('submit',event=>{
 event.preventDefault();const file=$('firmware').files[0];if(!file||busy)return;
 if(file.size>512*1048576){notice('固件包超过 512 MiB，请检查是否选中了完整烧录镜像。',true);return;}
 busy=true;controls();$('progress').hidden=false;$('progress').value=0;$('upload-status').textContent='正在上传…';
 const xhr=new XMLHttpRequest();xhr.open('POST','/api/upload');xhr.timeout=240000;xhr.setRequestHeader('Content-Type','application/octet-stream');xhr.setRequestHeader('X-OTA-Request','1');
 xhr.upload.onprogress=e=>{if(e.lengthComputable){const percent=Math.round(e.loaded/e.total*100);$('progress').value=percent;$('upload-status').textContent=percent===100?'上传完成，正在校验固件包…':`正在上传 ${percent}%`;}};
 xhr.onload=async()=>{try{const result=JSON.parse(xhr.responseText);if(xhr.status<200||xhr.status>=300)throw new Error(result.error||'上传失败');$('firmware').value='';$('file-info').textContent='';$('upload-status').textContent='校验通过，已保存为待发布版本。';notice('上传成功。请在待发布列表中点击“发布到设备”。');await state();}catch(error){$('upload-status').textContent='上传或校验失败。';notice(error.message,true);}finally{busy=false;controls();}};
 xhr.onerror=xhr.ontimeout=()=>{busy=false;controls();$('upload-status').textContent='上传未完成。';notice('网络连接失败或超时，请重试。',true);};xhr.send(file);
});
const tabs=[...document.querySelectorAll('[data-tab]')];
function selectTab(button,update=true){if(update&&location.hash!=='#'+button.dataset.tab)location.hash=button.dataset.tab;for(const tab of tabs){const selected=tab===button;tab.setAttribute('aria-selected',String(selected));tab.tabIndex=selected?0:-1;$(tab.getAttribute('aria-controls')).hidden=!selected;}}
for(const tab of tabs){tab.onclick=()=>selectTab(tab);tab.onkeydown=event=>{let index=tabs.indexOf(tab);if(event.key==='ArrowRight')index=(index+1)%tabs.length;else if(event.key==='ArrowLeft')index=(index+tabs.length-1)%tabs.length;else if(event.key==='Home')index=0;else if(event.key==='End')index=tabs.length-1;else return;event.preventDefault();selectTab(tabs[index]);tabs[index].focus();};}
for(const [tab,list] of Object.entries(lists)){
 const bar=document.createElement('div');bar.className='toolbar';
 const label=document.createElement('label');label.htmlFor='search-'+tab;label.textContent='筛选';
 const input=document.createElement('input');input.type='search';input.id=label.htmlFor;input.placeholder='版本、完整或短哈希、产品、状态、设备 IP / MAC / hostname…';
 try{input.value=sessionStorage.getItem(input.id)||'';}catch{}
 input.oninput=()=>{try{sessionStorage.setItem(input.id,input.value);}catch{}filterRows(tab);};
 const refresh=document.createElement('button');refresh.type='button';refresh.className='secondary';refresh.dataset.refresh=tab;refresh.textContent='刷新';
 const count=document.createElement('span');count.id='count-'+tab;count.className='filter-count';count.setAttribute('aria-live','polite');
 bar.append(label,input,refresh,count);const wrap=$(list).closest('.table-wrap');wrap.before(bar);
 const empty=document.createElement('p');empty.id='empty-'+tab;empty.className='empty';empty.textContent='暂无记录或没有符合筛选条件的记录。';empty.hidden=true;wrap.after(empty);
}


const sol={active:false,token:'',rows:[],text:'',cursor:0,generation:0,busy:false,console:new SolConsole()};
function solDecode(text){return new TextDecoder().decode(Uint8Array.from(text,c=>c.charCodeAt(0)));}
async function solGet(path){const r=await fetch(path,{cache:'no-store',headers:sol.token?{Authorization:'Bearer '+sol.token}:{}});const body=await r.json();if(!r.ok)throw Error(body.error||'日志请求失败');return body;}
function solRender(){const query=$('sol-filter').value.toLowerCase();$('sol-output').textContent=query?sol.text.split('\n').filter(line=>line.toLowerCase().includes(query)).join('\n'):sol.text;if($('sol-live').checked)$('sol-output').scrollTop=$('sol-output').scrollHeight;}
function solReset(){sol.generation++;sol.cursor=0;sol.text='';sol.console=new SolConsole();solRender();}
function solBoots(follow=false){const old=$('sol-boot').value;$('sol-boot').replaceChildren();for(const row of sol.rows.filter(r=>r.device===$('sol-device').value)){const o=document.createElement('option');o.value=row.boot;o.textContent=row.boot+(solDecode(row.meta).includes('current=yes')?'（当前）':'');$('sol-boot').append(o);}if(!follow&&[...$('sol-boot').options].some(o=>o.value===old))$('sol-boot').value=old;solReset();}
async function solIndex(rows=null,follow=false){if(!rows)rows=await solGet('/api/sol/devices');sol.rows=rows;const old=$('sol-device').value;$('sol-device').replaceChildren();for(const id of [...new Set(rows.map(r=>r.device))]){const o=document.createElement('option');o.value=id;o.textContent=id;$('sol-device').append(o);}if([...$('sol-device').options].some(o=>o.value===old))$('sol-device').value=old;solBoots(follow);if(!rows.length)$('sol-status').textContent='暂无设备日志，等待设备连接。';refreshFleetFromSol();}
async function solPoll(){
 if(sol.busy||!sol.active||location.hash!=='#sol'||!$('sol-live').checked)return;
 const device=$('sol-device').value,boot=$('sol-boot').value;if(!device||!boot)return;
 const generation=sol.generation;sol.busy=true;
 try{const rows=await solGet('/api/sol/logs/'+[device,boot,$('sol-stage').value,sol.cursor].map(encodeURIComponent).join('/'));if(generation!==sol.generation)return;
 for(const row of rows){sol.cursor=row.id;sol.console.push(row);}
 sol.text=sol.console.text($('sol-stage').value);
 solRender();const info=sol.rows.find(r=>r.device===device&&r.boot===boot);if(info){$('sol-meta').textContent='来源 IP='+info.peer+'\n'+solDecode(info.meta);$('sol-status').textContent=(Date.now()-info.seen*1000>60000?'设备暂未上报 · ':'已连接 · ')+'最后上报 '+new Date(info.seen*1000).toLocaleString()+(sol.console.truncated?' · 视图较早内容已截断':'');}
 }catch(e){$('sol-status').textContent=e.message;}finally{sol.busy=false;}
}
$('sol-connect').onclick=async()=>{sol.token=$('sol-key').value.trim();if(!/^[a-f0-9]{64}$/.test(sol.token)){$('sol-status').textContent='请输入 64 位查看凭据';sol.token='';sol.active=false;return;}sol.active=true;try{await solIndex();await solPoll();}catch(e){$('sol-status').textContent=e.message;}};
$('sol-disconnect').onclick=()=>{sol.active=false;sol.token='';$('sol-key').value='';solReset();$('sol-status').textContent='已断开';};
$('sol-device').onchange=()=>{solBoots();solPoll();};$('sol-boot').onchange=$('sol-stage').onchange=()=>{solReset();solPoll();};
$('sol-refresh').onclick=async()=>{if(!sol.active)return;try{await solIndex();await solPoll();}catch(e){$('sol-status').textContent=e.message;}};
$('sol-filter').oninput=solRender;$('sol-live').onchange=()=>solPoll();
$('sol-export').onclick=()=>{const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([$('sol-output').textContent],{type:'text/plain;charset=utf-8'}));a.download='boot-log-'+($('sol-boot').value||'empty')+'.txt';a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000);};
fetch('/api/sol/config',{cache:'no-store'}).then(r=>r.json()).then(async config=>{if(config.open_view){$('sol-auth').hidden=true;sol.active=true;$('sol-status').textContent='正在连接设备日志';await solIndex();await solPoll();}else $('sol-auth').hidden=false;}).catch(e=>{$('sol-status').textContent=e.message;});
setInterval(solPoll,2000);
setInterval(async()=>{if(sol.active&&location.hash==='#sol')try{const rows=await solGet('/api/sol/devices');const signature=a=>a.map(r=>r.device+'/'+r.boot).join('|');const selected=sol.rows.find(r=>r.device===$('sol-device').value&&r.boot===$('sol-boot').value);const follow=$('sol-live').checked&&!!selected&&solDecode(selected.meta).includes('current=yes');if(signature(rows)!==signature(sol.rows))await solIndex(rows,follow);else sol.rows=rows;}catch{}},10000);

function restoreTab(){
 const detail=location.pathname==='/firmware';
 const route=detail?'firmware':location.hash.slice(1)||'dashboard';
 const dashboard=route==='dashboard',devices=route==='devices',sol=route==='sol';
 $('dashboard-panel').hidden=!dashboard||detail;$('devices-panel').hidden=!devices||detail;
 $('sol-panel').hidden=!sol||detail;$('overview').hidden=dashboard||devices||sol||detail;
 const pages={dashboard:['总览','设备状态、固件发布与最近事件。'],devices:['设备','固件身份、网络信息与升级结果。'],sol:['远程日志 SOL','只读启动日志与历史回放。']};
 const [title,description]=pages[route]||['固件 OTA','上传固件、发布版本并追踪升级结果。'];
 $('module-title').textContent=detail?'固件详情':title;$('module-description').textContent=detail?'版本内容、下载量与设备升级统计。':description;
 for(const id of ['dashboard','devices','ota','sol'])$('module-'+id).setAttribute('aria-current',(!detail&&((id===route)||(id==='ota'&&!pages[route])))||(detail&&id==='ota')?'page':'false');
 selectTab(tabs.find(t=>t.dataset.tab===location.hash.slice(1))||tabs[0],false);
}
window.addEventListener('hashchange',restoreTab);restoreTab();
document.querySelectorAll('[data-refresh]').forEach(b=>b.onclick=()=>state().catch(e=>notice(e.message,true)));
$('header-refresh').onclick=()=>state().catch(e=>notice(e.message,true));
state().catch(e=>{notice(e.message,true);$('service-health').textContent='管理服务不可用';$('service-health').classList.add('error');});
