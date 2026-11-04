'use strict';
function incidentSolURL(entry,boot){
 const url=new URL('/',location.href);
 for(const [key,value] of Object.entries({sol_device:entry.device,sol_boot:boot,sol_publication:String(entry.publication),sol_source:entry.source_boot}))url.searchParams.set(key,value);
 url.hash='sol';return url.pathname+url.search+url.hash;
}
function incidentLogRow(entry,item,log,startup=false){
 const capture=log.capture||log.boot,base='/api/sol/incidents/'+[entry.device,entry.publication,entry.source_boot].map(encodeURIComponent).join('/');
 const row=document.createElement('div');row.className='incident-boot'+(startup?' incident-startup':'');
 const label=document.createElement('strong');label.textContent=(startup?(log.role==='failed-startup'?'前次异常启动':'回退后恢复启动'):(log.role==='target'?'升级后启动':'升级执行阶段'))+' · '+capture.slice(0,8)+' · '+log.bytes.toLocaleString()+' 字节';row.append(label);
 if(startup){
  const meta=document.createElement('p');meta.className='muted';meta.textContent='归档接收：'+(SolNames.stamp(log.received_at)?.label||'未知')+'；依据完整恢复启动 ID 和相邻缓存代次关联。';row.append(meta);
  const integrity=document.createElement('p');integrity.className='muted';integrity.textContent=log.complete?'四阶段长度 / CRC 已校验，缓存未报告丢失。':'存在缺失、截断或恢复前缀；以下为已保存片段。';row.append(integrity);
  const stages=document.createElement('p');stages.className='muted';stages.textContent=(log.stages||[]).map(s=>`${s.stage}: ${s.bytes} B · CRC ${s.crc32} · dropped ${s.dropped}`).join(' / ');row.append(stages);
 }
 const actions=document.createElement('div');actions.className='toolbar';row.append(actions);
 const solLink=document.createElement('a');solLink.className='incident-sol-link';solLink.textContent='在 SOL 中打开';solLink.href=incidentSolURL(entry,capture);actions.append(solLink);
 if(!log.bytes){const waiting=document.createElement('small');waiting.textContent='等待日志上传，或该启动未保存可用日志';row.append(waiting);return row;}
 const view=document.createElement('button'),download=document.createElement('button');view.type=download.type='button';view.className=download.className='secondary';view.textContent='查看日志';download.textContent='下载附件';actions.prepend(view,download);
 const output=document.createElement('pre');output.className='incident-console';output.hidden=true;
 const status=document.createElement('small');row.append(status,output);
 async function read(save){
  view.disabled=download.disabled=true;status.textContent='正在读取…';
  try{
   const decoder=new SolConsole(32*1024*1024);let cursor=0,finished=false;
   for(let i=0;i<256;i++){
    const response=await fetch(base+'/'+encodeURIComponent(capture)+'/'+cursor,{cache:'no-store'});
    if(!response.ok)throw Error('无法读取事故附件，请刷新后重试。');
    const rows=await response.json();if(!rows.length){finished=true;break;}
    for(const r of rows){if(r.id<=cursor)throw Error('事故日志游标异常');cursor=r.id;decoder.push(r);}
   }
   if(!finished)throw Error('日志分页超过读取上限。');
   const text=decoder.text('all');status.textContent=decoder.truncated?'视图存在缺口或截断':startup&&log.complete?'已加载经过长度 / CRC 校验的阶段归档':'已加载服务器保存片段，完整性未确认';
   if(save){
    const url=URL.createObjectURL(new Blob([text],{type:'text/plain;charset=utf-8'})),link=document.createElement('a');link.href=url;
    const stamp=startup?SolNames.stamp(log.received_at)?.file:null;
    link.download=`ota-${log.role}-${(item.tag||'unknown').replace(/[^a-zA-Z0-9.-]/g,'_')}-${(item.firmware_id||'unknown').slice(0,8)}-${stamp?stamp+'-':''}${capture.slice(0,8)}.log`;
    link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
   }else{output.textContent=text;output.hidden=false;}
  }catch(e){status.textContent=e.message;}finally{view.disabled=download.disabled=false;}
 }
 view.onclick=()=>read(false);download.onclick=()=>read(true);return row;
}
function solRequestedRecord(){
 const params=new URLSearchParams(location.search),device=params.get('sol_device'),boot=params.get('sol_boot');
 if(!device&&!boot)return null;
 const safe=s=>!!s&&s.length<=63&&/^[a-z0-9_-]+$/.test(s);
 if(!safe(device)||!safe(boot))return null;
 const publication=params.get('sol_publication'),source=params.get('sol_source');
 return {device,boot,...(/^[1-9][0-9]{0,9}$/.test(publication||'')&&Number(publication)<=2147483647&&safe(source)?{publication,source}:{})};
}
async function solAddIncidentFallback(rows){
 const requested=solRequestedRecord();if(!requested?.publication||rows.some(r=>r.device===requested.device&&r.boot===requested.boot))return rows;
 const path='/api/sol/incidents/'+[requested.device,requested.publication,requested.source].map(encodeURIComponent).join('/');
 const evidence=await solGet(path),startup=(evidence.startup_archives||[]).find(r=>r.capture===requested.boot),live=(evidence.boots||[]).find(r=>r.boot===requested.boot);
 if(!startup&&!live)return rows;
 const date=startup?.received_at||0,kind=startup?(startup.role==='failed-startup'?'previous-unconfirmed':'healthy-current'):null;
 return [...rows,{device:requested.device,boot:requested.boot,order:0,seen:date,archive_received_at:date,peer:'独立事故附件',incident:{publication:requested.publication,source:requested.source},meta:(kind?'startup='+kind+'\n':'')+'current=no\nincident-copy=yes\n'+(startup?'recovery-boot='+startup.recovery_boot+'\n':'')}];
}
