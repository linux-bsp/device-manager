'use strict';
const $ = id => document.getElementById(id);
let busy = false;
let refreshing = null;
function notice(message, error=false){$('notice').textContent=message;$('notice').className='notice'+(error?' error':'');$('notice').hidden=false;}
function size(bytes){return (bytes/1048576).toFixed(1)+' MiB';}
function uploadTime(item){return item.uploaded_at?DeviceTime.format(item.uploaded_at):'未知';}
function cell(row,text){const td=document.createElement('td');td.textContent=text;row.append(td);return td;}
function detailLink(item,tab=activeTab()){const a=document.createElement('a');a.textContent='查看详情';a.href='/firmware?kind='+((item.id||'').startsWith('draft-')?'draft':'release')+'&id='+encodeURIComponent(item.id)+'&tab='+tab;return a;}
function deleteFirmwareButton(item){
 const b=document.createElement('button');b.type='button';b.className='delete-firmware';b.textContent='删除';
 if(item.published||item.deleted){b.disabled=true;b.title=item.deleted?'固件包已删除，仅保留审计记录':'当前发布版本不能删除';return b;}
 const id=item.id.startsWith('draft-')?item.id:'release-'+item.id;b.dataset.delete=id;
 b.onclick=async()=>{
  if(busy||refreshing)return;
  if(!window.confirm(`彻底删除固件 ${item.tag||item.firmware_version} / ${(item.firmware_id||'').slice(0,8)}？\n将删除全部上传包和历史发布副本，仅保留审计记录。删除后无法下载或用于回退。`))return;
  busy=true;controls();
  try{
   const r=await fetch('/api/firmware/'+encodeURIComponent(id),{method:'DELETE',headers:{'X-OTA-Request':'1'}});
   const result=await r.json();if(!r.ok)throw new Error(result.error||'删除失败');
   notice('固件包及全部历史发布副本已删除，仅保留审计记录。');await state();
  }catch(e){notice(e.message,true);}finally{busy=false;controls();}
 };return b;
}
function identityCells(row,item){const hwid=item.hwid||item.hwids?.[0];cell(row,FirmwareHardware.product(hwid)).title=FirmwareHardware.describe(hwid);cell(row,item.tag||item.firmware_version||'未知');const td=cell(row,item.firmware_id?item.firmware_id.slice(0,8):'未知');td.title=item.firmware_id||'历史记录未保存固件 ID';}
function invalidButton(){const b=document.createElement('button');b.type='button';b.className='invalid';b.disabled=true;b.textContent='无效固件，禁止发布';b.title='固件已隔离，不能发布到设备';return b;}
function publishingButton(){const b=document.createElement('button');b.type='button';b.className='publishing';b.disabled=true;b.textContent='当前发布版本';return b;}
function activeTab(){return document.querySelector('[data-tab][aria-selected=true]')?.dataset.tab||'manage';}
function searchable(row,item){row.dataset.search=JSON.stringify({...item,hwidLabels:(item.hwids||item.firmware?.hwids||[]).map(id=>FirmwareHardware.describe(id))});}
const lists={manage:'drafts',history:'devices',releases:'releases',audit:'audit'};
function filterRows(tab){const tokens=$('search-'+tab).value.trim().toLowerCase().split(/\s+/).filter(Boolean);let visible=0,total=0;for(const row of $(lists[tab]).children){total++;const text=((row.dataset.search||'')+' '+row.textContent).toLowerCase();row.hidden=!tokens.every(t=>text.includes(t));if(!row.hidden)visible++;}$('count-'+tab).textContent=`${visible} / ${total} 条`;$('empty-'+tab).hidden=visible>0;}
function downloadLink(item){const link=document.createElement('a');link.className='download';link.textContent='下载固件包';link.href='/api/download/'+(item.id.startsWith('draft-')?'draft':'release')+'/'+encodeURIComponent(item.id);return link;}
function badge(td,text,current=false){const span=document.createElement('span');span.className='badge'+(current?' green':'');span.textContent=text;td.append(span);}
function controls(){ const disabled=busy||!!refreshing; $('upload-button').disabled=disabled||!$('firmware').files.length; $('firmware').disabled=disabled; document.querySelectorAll('[data-refresh]').forEach(b=>b.disabled=disabled); document.querySelectorAll('[data-publish],[data-delete]').forEach(b=>b.disabled=disabled); }
function state(){
 if(refreshing)return refreshing;
 refreshing=loadState().finally(()=>{refreshing=null;controls();});
 controls();return refreshing;
}
async function loadState(){
 const [response]=await Promise.all([fetch('/api/state',{cache:'no-store'}),FirmwareHardware.loadRegistry()]);if(!response.ok)throw new Error('无法读取版本列表，请检查服务日志。');
 const data=await response.json();
 renderFleet(data);
 $('service-health').textContent='管理服务已连接';$('service-health').classList.remove('error');
 if(location.pathname==='/firmware'){renderDetails(data);return;}
 // Resolve the active publication from the release list and include it in the
 // publishing table beside unpublished drafts.
 const phases={'requested':'请求升级','installing':'正在安装','rebooting':'等待重启','boot-testing':'启动健康检查中','success':'升级成功','install-failed':'安装失败','rolled-back':'已回滚','interrupted':'升级中断','reboot-failed':'重启失败','up-to-date':'无需升级','device-error':'设备异常','verify-failed':'固件校验失败','transport-error':'网络传输重试中','cancelled':'升级已取消'};
 const why=r=>r==='not-reported'?'原因未上报':r==='none'?'—':r||'未上报';
 const fmt=v=>v?`v${v>>>16}.${(v>>>8)&255}.${v&255}`:'未知';
 const when=v=>DeviceTime.format(v);
 const identity=d=>data.releases.find(r=>r.publication===d.publication)||{hwid:d.hwid,tag:fmt(d.version)};
 for(const [tab,list] of Object.entries(lists))$(list).replaceChildren();
 $('drafts-empty').hidden=true;$('releases-empty').hidden=true;
 const candidates=new Map();
 const firmwareKey=item=>item.firmware_id?JSON.stringify([item.tag||item.firmware_version,item.firmware_id]):item.id;
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
   identityCells(row,d);cell(row,size(d.bytes));cell(row,uploadTime(d));
   const action=cell(row,'');
   publicationControls(action,d,data);
   cell(row,'').append(detailLink(d,tbody==='drafts'?'manage':'releases'));
   if(tbody==='drafts')cell(row,'').append(deleteFirmwareButton(d));
   searchable(row,{...d,status:d.deleted?'固件包已删除':!d.valid?'无效':d.published?'已发布':'未发布',created_text:when(d.created)});$(tbody).append(row);
  }
 }
 for(const d of [...(data.device_history||[])].sort((a,b)=>b.updated-a.updated||b.publication-a.publication)){
  const item=identity(d),row=document.createElement('tr');identityCells(row,item);cell(row,deviceLabel(d,true)).title=d.device;
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
  const item=identity(e),row=document.createElement('tr');identityCells(row,item);cell(row,when(e.timestamp));cell(row,deviceLabel(e,true)).title=e.device;cell(row,`${e.category==='device-error'?'设备异常':'升级失败'} / ${why(e.reason)}`);
  const td=cell(row,actions[e.action]||e.action);td.className='network-info';const details=document.createElement('details');const title=document.createElement('summary');title.textContent='查看溯源';details.append(title);
  for(const text of [`发布记录 ${e.publication} · 回退来源 ${e.source_publication||'无'} · 回退发布 ${e.replacement_publication||'无'}`,e.kernel_sha256,e.rootfs_sha256,e.boot]){const line=document.createElement('div');line.textContent=text;details.append(line);}td.append(details);if(item.id)td.append(detailLink(item,'audit'));searchable(row,{...e,firmware:item});$('audit').append(row);
 }
 for(const tab of Object.keys(lists))filterRows(tab);
 controls();
}

const diagnosticReasons={
 'linux-panic-observed':'检测到 Linux panic 痕迹（未确认对应哪次启动）',
 'linux-oops-observed':'检测到 Linux Oops 痕迹（未确认对应哪次启动）',
 'data-unavailable-observed':'检测到 data 挂载或存储状态不可用',
 'data-read-only-observed':'检测到 data 为只读',
 'data-low-space-observed':'检测到 data 可用空间不足',
 'data-unreadable-observed':'检测到 data 日志目录不可读取',
 'boot-failed-rolled-back':'新固件启动失败，已回滚；具体根因未确认',
 'transaction-interrupted':'升级事务中断；具体根因未确认',
 'install-command-failed':'固件安装命令失败',
 'not-reported':'设备未上报具体原因',none:'设备未提供额外原因'
};
function incidentEvidence(entry,item){
 const details=document.createElement('details');details.className='incident-evidence';
 const summary=document.createElement('summary');summary.textContent=`${deviceLabel(entry,true)} · ${entry.phase} · ${DeviceTime.format(entry.updated)} · 查看原因与日志`;details.append(summary);
 const reason=document.createElement('p');reason.textContent=diagnosticReasons[entry.reason]||entry.reason||'设备未上报具体原因';details.append(reason);
 const pane=document.createElement('div');details.append(pane);let loaded=false;
 const base='/api/sol/incidents/'+[entry.device,entry.publication,entry.source_boot].map(encodeURIComponent).join('/');
 details.ontoggle=async()=>{
  if(!details.open||loaded)return;loaded=true;pane.textContent='正在读取诊断记录…';
  try{
   const response=await fetch(base,{cache:'no-store'});
   if(response.status===404){pane.textContent='此事件尚无关联日志。';return;}
   if(!response.ok)throw Error('诊断记录暂时不可用，请重试。');
   const evidence=await response.json();pane.replaceChildren();
   if(evidence.reason&&evidence.reason!=='none'&&evidence.reason!=='not-reported')reason.textContent=diagnosticReasons[evidence.reason]||evidence.reason;
   const archives=evidence.startup_archives||[],info=document.createElement('p');pane.append(info);
   if(evidence.mode==='candidate'){info.textContent='升级结果已记录，日志关联待处理；稍后刷新重试。';return;}
   if(evidence.mode==='reason-only'&&!archives.length){info.textContent='已记录原因，尚无可确认关联的日志。观察记录启动：'+(evidence.observed_boot||'未知');return;}
   info.textContent=archives.length?'启动日志已独立保存为事故附件；异常启动与恢复启动分别列出。':'保留服务器已接收的日志片段，完整性未确认。';
   for(const archive of archives)pane.append(incidentLogRow(entry,item,archive,true));
   for(const boot of evidence.boots)pane.append(incidentLogRow(entry,item,boot));
  }catch(e){pane.textContent=e.message;loaded=false;}
 };
 return details;
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
 if(!item.deleted)pane.append(downloadLink(item));
 const publications=data.releases.filter(x=>x.firmware_id===item.firmware_id);
 const ids=new Set(publications.map(x=>x.publication));
 const history=(data.device_history||[]).filter(x=>ids.has(x.publication));
 const downloads=(data.download_stats||[]).filter(x=>ids.has(x.publication));
 const sum=key=>downloads.reduce((n,x)=>n+(x[key]||0),0);
 const success=history.filter(x=>x.phase==='success').length;
 const failures=history.filter(x=>x.category==='upgrade-failure');
 const incidents=history.filter(x=>x.category==='device-error');
 const active=publications.filter(x=>x.published).map(x=>x.hwid);
 const table=document.createElement('table');pane.append(table);
 const rows=[['上传时间',uploadTime(item)],
 ['版本号',item.tag||item.firmware_version],['内容哈希',item.firmware_id],['产品信息',FirmwareHardware.product(item.hwid||item.hwids?.[0])],
 ['固件大小',size(item.bytes)],['发布状态',active.length?'当前 HWID 通道：'+active.join('、'):'未发布'],
 ['是否有效',item.deleted?'固件包已删除，仅保留审计记录':item.valid?'有效（上传校验通过，未被隔离）':'无效（已隔离，禁止发布）'],
 ['设备下载次数',String(sum('identified_downloads'))],['未识别设备的下载次数',String(sum('downloads')-sum('identified_downloads'))],
 ['更新成功次数',String(success)],['更新失败次数',String(failures.length)],['设备异常次数',String(incidents.length)]];
 for(const [key,value] of rows){const row=document.createElement('tr');cell(row,key);const td=cell(row,value);td.style.whiteSpace='normal';td.style.overflowWrap='anywhere';table.append(row);}
 const hwidRow=document.createElement('tr');cell(hwidRow,'适用 HWID');const hardware=cell(hwidRow,'');hardware.style.whiteSpace='normal';hardware.style.overflowWrap='anywhere';
 for(const id of item.hwids||[]){const entry=document.createElement('div');entry.className='firmware-hwid';const code=document.createElement('code');code.textContent=id;const fields=document.createElement('div');fields.className='muted';fields.textContent=FirmwareHardware.describe(id);entry.append(code,fields);hardware.append(entry);}
 if(!hardware.childElementCount)hardware.textContent='未声明（历史固件，无法按 HWID 核对）';
 table.insertBefore(hwidRow,table.children[4]);
 const note=document.createElement('p');note.className='muted';note.textContent='统计按相同内容哈希汇总。下载按 rootfs.itb 完整响应计次，重试和恢复下载会再次计数；不代表安装成功。未携带设备 ID 的请求单列，旧版本启用统计前的下载无法追溯。升级结果按每次升级事务最终状态计数，成功必须经过重启健康确认。';pane.append(note);
 const heading=document.createElement('h2');heading.textContent='失败与设备异常原因';pane.append(heading);
 const errors=failures.concat(incidents).sort((a,b)=>b.updated-a.updated);
 for(const entry of errors)pane.append(incidentEvidence(entry,item));
 if(!errors.length){const line=document.createElement('p');line.textContent='暂无失败或设备异常记录。';pane.append(line);}
 const action=document.createElement('div');action.className='publication-controls';pane.append(action);publicationControls(action,item,data);
 action.append(deleteFirmwareButton(item));
 controls();
}
function publicationControls(container,item,data){
 if(item.deleted){container.textContent='固件包已删除';return;}
 const source=(item.id||'').startsWith('draft-')?item:data.drafts.find(x=>x.valid&&x.firmware_id===item.firmware_id);
 if(!item.valid){container.append(invalidButton());return;}
 const hwids=item.hwids||[];
 if(!hwids.length){container.textContent='未声明适用 HWID，不能发布';return;}
 const active=hwids.every(hwid=>data.releases.some(x=>x.published&&x.hwid===hwid&&x.firmware_id===item.firmware_id));
 if(active){container.append(publishingButton());return;}
 if(!source){container.textContent='未发布';return;}
 const button=document.createElement('button');button.type='button';button.textContent='发布到设备';button.dataset.publish=source.id;
 button.title=`按固件包声明发布到全部 ${hwids.length} 个 HWID，不能手动修改适用硬件`;
 button.onclick=()=>publish(source.id);container.append(button);
}
async function publish(id){
 busy=true;controls();
 try{const r=await fetch('/api/publish/'+id,{method:'POST',headers:{'X-OTA-Request':'1'}});const result=await r.json();if(!r.ok)throw new Error(result.error||'发布失败');notice('已发布，适用设备由固件包内的 HWID 声明确定。');await state();}
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


const sol={active:false,token:'',rows:[],text:'',cursor:0,generation:0,busy:false,deepLinkApplied:false,console:new SolConsole()};
function solDecode(text){return new TextDecoder().decode(Uint8Array.from(text,c=>c.charCodeAt(0)));}
async function solGet(path){const r=await fetch(path,{cache:'no-store',headers:sol.token?{Authorization:'Bearer '+sol.token}:{}});const body=await r.json();if(!r.ok)throw Error(body.error||'日志请求失败');return body;}
function solRender(){const query=$('sol-filter').value.toLowerCase();$('sol-output').textContent=query?sol.text.split('\n').filter(line=>line.toLowerCase().includes(query)).join('\n'):sol.text;if($('sol-live').checked)$('sol-output').scrollTop=$('sol-output').scrollHeight;}
function solReset(){sol.generation++;sol.cursor=0;sol.text='';sol.console=new SolConsole();solRender();}
function solBoots(follow=false){const old=$('sol-boot').value;$('sol-boot').replaceChildren();for(const row of sol.rows.filter(r=>r.device===$('sol-device').value)){const o=document.createElement('option');o.value=row.boot;o.textContent=SolNames.label(row);o.title=SolNames.archived(row)?'服务器首次接收归档时间（北京时间），记录 ID：'+row.boot:'启动 ID：'+row.boot;$('sol-boot').append(o);}if(!follow&&[...$('sol-boot').options].some(o=>o.value===old))$('sol-boot').value=old;solReset();solRecordOptions();}
async function solIndex(rows=null,follow=false){
 if(!rows)rows=await solGet('/api/sol/devices');rows=await solWithBundles(rows);rows=await solAddIncidentFallback(rows);sol.rows=rows;
 const old=$('sol-device').value,requested=solRequestedRecord();$('sol-device').replaceChildren();
 for(const id of [...new Set(rows.map(r=>r.device))]){const o=document.createElement('option');o.value=id;o.textContent=deviceLabel({device:id});o.title=id;$('sol-device').append(o);}
 if([...$('sol-device').options].some(o=>o.value===old))$('sol-device').value=old;
 if(requested&&!sol.deepLinkApplied&&rows.some(r=>r.device===requested.device))$('sol-device').value=requested.device;
 solBoots(follow);
 if(requested&&!sol.deepLinkApplied){
  $('sol-boot').value=requested.boot;solReset();solRecordOptions();sol.deepLinkApplied=true;
  if(!$('sol-boot').value)$('sol-status').textContent='指定启动记录不存在或已删除，且没有可用事故附件。';
 }
 if(!rows.length)$('sol-status').textContent='暂无设备日志，等待设备连接。';refreshFleetFromSol();
}
async function solPoll(){
 if(sol.busy||!sol.active||location.hash!=='#sol'||!$('sol-live').checked)return;
 const device=$('sol-device').value,boot=$('sol-boot').value;if(!device||!boot)return;
 const generation=sol.generation;sol.busy=true;
 try{const selected=sol.rows.find(r=>r.device===device&&r.boot===boot);if(selected?.bundleOnly){$('sol-status').textContent='完整日志包已归档；实时文本窗口已淘汰，请下载日志包。';$('sol-meta').textContent='归档接收：'+DeviceTime.format(selected.bundle.received)+'\nSHA256='+selected.bundle.hash;return;}const endpoint=selected?.incident?'/api/sol/incidents/'+[device,selected.incident.publication,selected.incident.source,boot,sol.cursor].map(encodeURIComponent).join('/'):'/api/sol/logs/'+[device,boot,$('sol-stage').value,sol.cursor].map(encodeURIComponent).join('/');const rows=await solGet(endpoint);if(generation!==sol.generation)return;
 for(const row of rows){sol.cursor=row.id;sol.console.push(row);}
 sol.text=sol.console.text($('sol-stage').value);
 solRender();const info=sol.rows.find(r=>r.device===device&&r.boot===boot);if(info){$('sol-meta').textContent=(SolNames.archived(info)?'归档名='+SolNames.filename(info,'all')+'\n时间来源=服务器首次接收（北京时间），不代表异常发生时间\n':'')+(info.incident?'数据来源=独立事故附件（原 SOL 记录已删除）\n':'来源 IP='+info.peer+'\n')+solDecode(info.meta);$('sol-status').textContent=(info.incident?'独立事故归档 · '+boot:SolNames.archived(info)?'已归档 · 服务器接收时间 '+(SolNames.stamp(info.archive_received_at??info.seen)?.label||'未知'):(Date.now()-info.seen*1000>60000?'设备暂未上报 · ':'已连接 · ')+'最后上报 '+DeviceTime.format(info.seen))+(sol.console.truncated?' · 视图较早内容已截断':'');}
 }catch(e){$('sol-status').textContent=e.message;}finally{sol.busy=false;}
}
$('sol-connect').onclick=async()=>{sol.token=$('sol-key').value.trim();if(!/^[a-f0-9]{64}$/.test(sol.token)){$('sol-status').textContent='请输入 64 位查看凭据';sol.token='';sol.active=false;return;}sol.active=true;try{await solIndex();await solPoll();}catch(e){$('sol-status').textContent=e.message;}};
$('sol-disconnect').onclick=()=>{sol.active=false;sol.token='';$('sol-key').value='';solReset();$('sol-status').textContent='已断开';};
$('sol-device').onchange=()=>{solBoots();solPoll();};$('sol-boot').onchange=$('sol-stage').onchange=()=>{solReset();solRecordOptions();solPoll();};
$('sol-refresh').onclick=async()=>{if(!sol.active)return;try{await solIndex();await solPoll();}catch(e){$('sol-status').textContent=e.message;}};
$('sol-filter').oninput=solRender;$('sol-live').onchange=()=>solPoll();
$('sol-bundle').onclick=solDownloadBundle;
$('sol-export').onclick=()=>{const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([$('sol-output').textContent],{type:'text/plain;charset=utf-8'}));const row=sol.rows.find(r=>r.device===$('sol-device').value&&r.boot===$('sol-boot').value);a.download=SolNames.filename(row,$('sol-stage').value);a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000);};
fetch('/api/sol/config',{cache:'no-store'}).then(r=>r.json()).then(async config=>{if(config.open_view){$('sol-auth').hidden=true;sol.active=true;$('sol-status').textContent='正在连接设备日志';await solIndex();await solPoll();}else $('sol-auth').hidden=false;}).catch(e=>{$('sol-status').textContent=e.message;});
setInterval(solPoll,2000);
setInterval(async()=>{if(sol.active&&location.hash==='#sol')try{const rows=await solWithBundles(await solGet('/api/sol/devices'));const signature=a=>a.map(r=>r.device+'/'+r.boot).join('|');const selected=sol.rows.find(r=>r.device===$('sol-device').value&&r.boot===$('sol-boot').value);const follow=$('sol-live').checked&&!!selected&&solDecode(selected.meta).includes('current=yes');if(signature(rows)!==signature(sol.rows))await solIndex(rows,follow);else {sol.rows=rows;solRecordOptions();for(const option of $('sol-device').options)option.textContent=deviceLabel({device:option.value});}}catch{}},10000);

function restoreTab(){
 const detail=location.pathname==='/firmware';
 const route=detail?'firmware':location.hash.slice(1)||'dashboard';
 const dashboard=route==='dashboard',devices=route==='devices',sol=route==='sol',clock=route==='clock';
 $('dashboard-panel').hidden=!dashboard||detail;$('devices-panel').hidden=!devices||detail;
 $('clock-panel').hidden=!clock||detail;$('sol-panel').hidden=!sol||detail;$('overview').hidden=dashboard||devices||sol||clock||detail;
 const pages={clock:['时间同步','时间状态、NTP / HTTP 时间源与远程校准。'],dashboard:['总览','设备状态、固件发布与最近事件。'],devices:['设备','固件身份、网络信息与升级结果。'],sol:['远程日志 SOL','只读启动日志与历史回放。']};
 const [title,description]=pages[route]||['固件 OTA','上传固件、发布版本并追踪升级结果。'];
 $('module-title').textContent=detail?'固件详情':title;$('module-description').textContent=detail?'版本内容、下载量与设备升级统计。':description;
 for(const id of ['dashboard','devices','ota','sol','clock'])$('module-'+id).setAttribute('aria-current',(!detail&&((id===route)||(id==='ota'&&!pages[route])))||(detail&&id==='ota')?'page':'false');
 selectTab(tabs.find(t=>t.dataset.tab===location.hash.slice(1))||tabs[0],false);
}
window.addEventListener('hashchange',restoreTab);restoreTab();
document.querySelectorAll('[data-refresh]').forEach(b=>b.onclick=()=>state().catch(e=>notice(e.message,true)));
$('header-refresh').onclick=()=>{if(location.hash==='#clock')clockRefresh().catch(e=>notice(e.message,true));else state().catch(e=>notice(e.message,true));};
state().catch(e=>{notice(e.message,true);$('service-health').textContent='管理服务不可用';$('service-health').classList.add('error');});
