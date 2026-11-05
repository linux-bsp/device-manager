'use strict';
const clockView={data:null,received:0,dirty:false,loading:null,busy:false};
const clockWhen=seconds=>DeviceTime.format(seconds,'未校准');
const clockFields=report=>Object.fromEntries((report||'').split('\n').slice(1).filter(line=>line.includes('=')).map(line=>[line.slice(0,line.indexOf('=')),line.slice(line.indexOf('=')+1)]));
function clockRow(){return clockView.data?.devices.find(row=>row.device===$('clock-device').value);}
function clockProtocol(){
 const type=$('clock-source-type').value;const manager=type==='manager';$('clock-source').disabled=manager;
 $('clock-source').placeholder=type==='ntp'?'ntp://192.168.10.1:123':'http://192.168.10.1:8090';
 $('clock-source-help').textContent=manager?'使用设备当前配置的管理服务器。':type==='ntp'?'填写 NTP 服务器域名或 IP，可指定 UDP 端口（默认 123）。':'对方必须提供 Device Manager 的 /api/time 接口；普通网站不能作为时间源。';
}
function clockForm(row){
 const source=row?.source||'-';$('clock-source-type').value=source==='-'?'manager':source.startsWith('ntp://')?'ntp':'http';
 $('clock-source').value=source==='-'?'':source;$('clock-interval').value=row?.interval||3600;$('clock-enabled').checked=row?.enabled!==0;
 clockProtocol();clockView.dirty=false;
}
function clockPresence(row,now){
 const age=now-(row?.seen||0);
 $('clock-online').textContent=row?.seen?(age<=30?'最近有上报':'上报已过期'):'未收到校时上报';
 $('clock-online').className='pill '+(row?.seen&&age<=30?'good':'warn');
}
function clockTick(){
 const data=clockView.data;if(!data)return;
 const elapsed=Math.max(0,(performance.now()-clockView.received)/1000);
 $('clock-server-time').textContent=clockWhen(data.server_time+elapsed);
 const row=clockRow(),header=(row?.report||'').split('\n')[0].split(' ');const deviceTime=Number(header[4]);
 clockPresence(row,data.server_time+elapsed);
 $('clock-device-time').textContent=row?.report?clockWhen(deviceTime):'未上报';
 $('clock-time-hint').textContent=row?.report?'设备在最近一次上报时的时间；不实时外推':'等待设备回报时间状态';
}
function clockRender(){
 const row=clockRow(),fields=clockFields(row?.report),header=(row?.report||'').split('\n')[0].split(' ');
 const states={synchronized:'已同步',holdover:'保持上次时间（当前校时失败）','waiting-network':'等待网络',unsynchronized:'未同步',disabled:'自动校时已关闭'};
 const failures={1:'没有设时权限',2:'持久化目录不存在',5:'存储 I/O 错误',11:'网络尚未就绪或另一个校时正在执行',13:'配置目录不可写',22:'时间源配置无效',30:'存储只读或使用 RAM 临时目录',74:'时间源响应未通过校验',110:'时间源请求超时',113:'时间源不可达或 HTTP 请求失败'};
 const rtc={written:'已写入（不保证跨复位保时）','write-failed':'写入失败',unavailable:'不可用'};
 const lines=[['同步状态',states[fields['clock-state']]||'未上报'],['生效时间源',fields['clock-effective-source']||fields['clock-server']||'未上报'],['实际协议',fields['clock-source']==='ntp'?'NTP / UDP':fields['clock-source']==='device-manager'?'Device Manager / HTTP':'未上报'],['生效周期',fields['clock-interval']?fields['clock-interval']+' 秒':'未上报'],['最近成功校时',clockWhen(Number(fields['clock-synced-at']))],['最近修正量',fields['clock-offset-ms']!==undefined?fields['clock-offset-ms']+' ms':'未上报'],['网络延迟估计',fields['clock-uncertainty-ms']!==undefined?fields['clock-uncertainty-ms']+' ms':'未上报'],['RTC',rtc[fields['rtc-state']]||'未上报'],['最后上报',row?.seen?clockWhen(row.seen):'未上报'],['设备 IP',row?.peer||'未上报']];
 const dl=$('clock-status');dl.replaceChildren();for(const [label,value] of lines){const dt=document.createElement('dt'),dd=document.createElement('dd');dt.textContent=label;dd.textContent=value;dl.append(dt,dd);}
 $('clock-request-status').textContent=row?.revision?(!row.completed?'配置已提交，等待设备应用与回报。':row.error?'设备执行失败：'+(failures[row.error]||'errno '+row.error)+'；查看当前同步状态，或修正时间源后重试。':row.enabled?'最近配置请求已执行成功；当前同步状态以上方设备回报为准。':'设备已确认关闭自动校时。'):'使用设备默认配置；尚未下发网页配置。';
 if(row?.revision&&row.completed&&Number(header[1])!==row.revision)$('clock-request-status').textContent+=' 当前回报未确认该配置已持久化。';
 if(!clockView.dirty)clockForm(row);clockTick();
 $('clock-save').disabled=$('clock-sync').disabled=clockView.busy||!$('clock-device').value;
}
async function clockRefresh(){
 if(clockView.loading)return clockView.loading;
 clockView.loading=(async()=>{
  const r=await fetch('/api/clock',{cache:'no-store'});if(!r.ok)throw new Error('无法读取设备时间状态（HTTP '+r.status+'）');
  const data=await r.json();clockView.data=data;clockView.received=performance.now();
  const select=$('clock-device'),selected=select.value;
  const ids=[...new Set([...data.devices.map(row=>row.device),...(fleetState?.devices||[]).map(row=>row.device),...(sol.rows||[]).map(row=>row.device)])].sort();
  select.replaceChildren();for(const id of ids){const option=document.createElement('option');option.value=id;option.textContent=deviceLabel(data.devices.find(row=>row.device===id)||{device:id});option.title=id;select.append(option);}
  if(ids.includes(selected))select.value=selected;
  clockRender();
 })().finally(()=>{clockView.loading=null;});return clockView.loading;
}
function clockInput(){
 const type=$('clock-source-type').value;let source=$('clock-source').value.trim();
 if(type==='manager')source='-';else if(type==='ntp'&&!source.startsWith('ntp://'))source='ntp://'+source;
 if(source!=='-'){
  let url;try{url=new URL(source);}catch{throw new Error('请输入有效的时间服务器地址。');}
  if((type==='ntp'&&url.protocol!=='ntp:')||(type==='http'&&!['http:','https:'].includes(url.protocol))||url.username||url.password||url.search||url.hash||(url.pathname&&url.pathname!=='/'))throw new Error('时间源只能包含协议、主机和端口。');
  source=source.replace(/\/$/,'');
 }
 const interval=Number($('clock-interval').value);if(!Number.isInteger(interval)||interval<60||interval>86400)throw new Error('校时间隔需要在 60–86400 秒之间。');
 return {source,enabled:$('clock-enabled').checked?1:0,interval};
}
async function clockSubmit(sync){
 if(clockView.busy)return;
 try{
  const config=clockInput();if(sync&&!config.enabled)throw new Error('请启用自动校时后再发起立即校时。');
  const device=$('clock-device').value;if(!device)throw new Error('请选择设备。');
  clockView.busy=true;clockRender();
  const response=await fetch('/api/clock/config/'+encodeURIComponent(device),{method:'POST',headers:{'Content-Type':'text/plain','X-OTA-Request':'1'},body:`DMCLOCK1 ${config.enabled} ${config.interval} ${config.source}\n`});
  if(!response.ok)throw new Error('下发失败（HTTP '+response.status+'），请检查地址与服务状态。');
  clockView.dirty=false;$('clock-message').textContent='请求已保存，等待设备拉取并回报；提交成功不代表设备已完成校时。';await clockRefresh();
 }catch(e){$('clock-message').textContent=e.message;}finally{clockView.busy=false;clockRender();}
}
$('clock-form').onsubmit=e=>{e.preventDefault();clockSubmit(false);};$('clock-sync').onclick=()=>clockSubmit(true);
$('clock-form').oninput=()=>{clockView.dirty=true;clockProtocol();};
$('clock-device').onchange=()=>{clockView.dirty=false;clockRender();};
$('clock-refresh').onclick=()=>clockRefresh().catch(e=>{$('clock-message').textContent=e.message;});
window.addEventListener('hashchange',()=>{if(location.hash==='#clock')clockRefresh().catch(e=>{$('clock-message').textContent=e.message;});});
setInterval(()=>{if(location.hash==='#clock')clockRefresh().catch(e=>{$('clock-message').textContent=e.message;});},5000);
setInterval(clockTick,1000);
if(location.hash==='#clock')clockRefresh().catch(e=>{$('clock-message').textContent=e.message;});
