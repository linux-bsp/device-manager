'use strict';
let solDeleting=false;
function solRecordProtected(row){
 if(row.incident)return true;
 if(row.bundleOnly)return false;
 const meta=solDecode(row.meta||'');
 const latest=Math.max(0,...sol.rows.filter(r=>r.device===row.device&&!SolNames.archived(r)).map(r=>r.order));
 return !SolNames.archived(row)&&(meta.split('\n').includes('current=yes')||(!meta&&row.order===latest));
}
function solRecordOptions(){
 solBundleButton();
 const device=$('sol-device').value,selected=$('sol-boot').value;
 const rows=sol.rows.filter(row=>row.device===device),list=$('sol-boot-options');list.replaceChildren();
 const current=rows.find(row=>row.boot===selected);
 $('sol-boot-toggle').textContent=(current?SolNames.label(current):'暂无启动记录')+' ▾';
 $('sol-boot-toggle').title=current?.boot||'';
 for(const row of rows){
  const li=document.createElement('li'),choose=document.createElement('button'),remove=document.createElement('button');
  choose.type=remove.type='button';choose.className='sol-boot-choice';choose.textContent=SolNames.label(row);choose.title=row.boot;
  choose.setAttribute('aria-pressed',row.boot===selected?'true':'false');choose.dataset.boot=row.boot;
  choose.onclick=()=>{$('sol-boot').value=row.boot;$('sol-boot').dispatchEvent(new Event('change'));solRecordMenu(false);$('sol-boot-toggle').focus();};
  remove.className='sol-boot-delete';remove.textContent='×';remove.dataset.deleteBoot=row.boot;
  remove.setAttribute('aria-label','删除启动记录 '+SolNames.label(row));remove.disabled=solDeleting||solRecordProtected(row);
  remove.title=row.incident?'独立事故附件保留用于 OTA 审计':solRecordProtected(row)?'正在采集的本次启动记录不能删除':'删除此条服务器 SOL 历史记录';
  remove.onclick=()=>solRemoveRecord(row);li.append(choose,remove);list.append(li);
 }
 if(!rows.length){const empty=document.createElement('li');empty.className='muted';empty.textContent='暂无启动记录';list.append(empty);}
}
function solRecordMenu(open){$('sol-boot-menu').hidden=!open;$('sol-boot-toggle').setAttribute('aria-expanded',String(open));}
async function solRemoveRecord(row){
 if(solDeleting||solRecordProtected(row))return;
 if(!window.confirm('删除启动记录“'+SolNames.label(row)+'”？\n这会删除服务器上的该条 SOL 记录和正文，无法恢复。设备本地日志与 OTA 审计附件会保留。'))return;
 solDeleting=true;solRecordOptions();
 try{
  const response=await fetch('/api/sol/records/'+[row.device,row.boot].map(encodeURIComponent).join('/'),{method:'DELETE',headers:{'X-OTA-Request':'1',...(sol.token?{Authorization:'Bearer '+sol.token}:{})}});
  const result=await response.json();if(!response.ok)throw Error(result.error||'删除失败，请刷新后重试。');
  await solIndex();await solPoll();
 }catch(e){$('sol-status').textContent=e.message;}
 finally{solDeleting=false;solRecordOptions();}
}
$('sol-boot-toggle').onclick=()=>{solRecordOptions();solRecordMenu($('sol-boot-menu').hidden);};
document.addEventListener('click',e=>{if(!e.target.closest('.sol-boot-picker'))solRecordMenu(false);});
document.querySelector('.sol-boot-picker').addEventListener('keydown',e=>{if(e.key==='Escape'){solRecordMenu(false);$('sol-boot-toggle').focus();} });
