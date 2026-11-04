'use strict';

let fleetState = null;
let selectedDevice = '';
const phaseNames = {
  requested: '请求升级', installing: '正在安装', rebooting: '等待重启',
  'boot-testing': '健康检查中', success: '升级成功', 'install-failed': '安装失败',
  'rolled-back': '已回滚', interrupted: '升级中断', 'reboot-failed': '重启失败',
  'up-to-date': '版本一致', 'device-error': '设备异常', 'verify-failed': '校验失败'
};
function fleetTime(seconds) { return seconds ? new Date(seconds * 1000).toLocaleString() : '未上报'; }
function fleetPhase(phase) { return phaseNames[phase] || phase || '未上报'; }
function fleetTone(phase) {
  if (phase === 'success' || phase === 'up-to-date') return 'good';
  if (phase === 'device-error' || /failed|rolled-back|interrupted/.test(phase || '')) return 'bad';
  return 'warn';
}
function fleetPill(parent, value, tone) {
  const pill = document.createElement('span');
  pill.className = 'pill ' + tone;
  pill.textContent = value;
  parent.append(pill);
  return pill;
}
function fleetNetwork(history) {
  const records = (history?.network || []).filter(row => row.ip || row.mac || row.hostname);
  return [...records].sort((a,b) => b.last_seen - a.last_seen)[0] || null;
}
function fleetDevices(data) {
  const byId = new Map();
  for (const item of data.devices || []) byId.set(item.device, {...item});
  for (const item of sol.rows || []) {
    const existing = byId.get(item.device) || {device:item.device};
    if (!existing.solSeen || item.seen > existing.solSeen) existing.solSeen = item.seen;
    byId.set(item.device, existing);
  }
  return [...byId.values()].map(item => {
    const history = [...(data.device_history || [])].filter(row => row.device === item.device)
      .sort((a,b) => b.updated - a.updated)[0];
    const network = fleetNetwork(history);
    const release = (data.releases || []).find(row => row.publication === item.publication);
    return {...item, history, network, release, seen: Math.max(item.updated || 0,item.solSeen || 0)};
  }).sort((a,b) => b.seen - a.seen || a.device.localeCompare(b.device));
}
function fleetText(parent, tag, value, className) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  element.textContent = value;
  parent.append(element);
  return element;
}
function renderDeviceDetail(device, data) {
  const panel = $('device-detail');
  panel.replaceChildren();
  if (!device) { panel.hidden = true; return; }
  panel.hidden = false;
  const head = fleetText(panel,'h3','设备 '+device.device);
  head.className = 'device-id';
  const dl = document.createElement('dl'); panel.append(dl);
  const values = [
    ['产品信息', FirmwareHardware.product(device.hwid)],
    ['HWID',device.hwid||'未知'],
    ['固件版本', device.release?.tag || '未知'],
    ['固件 ID', device.release?.firmware_id || '未知'],
    ['IP 地址', device.network?.ip || '未上报'],
    ['MAC 地址', device.network?.mac || '未上报'],
    ['Hostname', device.network?.hostname || '未上报'],
    ['最近 OTA 结果', fleetPhase(device.phase)],
    ['最后上报', fleetTime(device.seen)],
    ['日志接收', device.solSeen ? fleetTime(device.solSeen) : '暂无记录']
  ];
  for (const [label,value] of values) {
    const item = document.createElement('div');
    fleetText(item,'dt',label);fleetText(item,'dd',value);
    dl.append(item);
  }
  const links = document.createElement('div');links.className='quick-links';panel.append(links);
  for (const [label,href] of [['查看升级历史','/#history'],['打开 SOL 日志','/#sol']]) {
    const a=document.createElement('a');a.href=href;a.textContent=label;links.append(a);
  }
  const transactions=(data.device_history||[]).filter(row=>row.device===device.device)
    .sort((a,b)=>b.updated-a.updated).slice(0,5);
  if(transactions.length){
    fleetText(panel,'h3','最近升级事务');
    const list=document.createElement('ul');list.className='activity';panel.append(list);
    for(const item of transactions){const row=document.createElement('li');
      const dot=document.createElement('span');dot.className='dot '+fleetTone(item.phase);row.append(dot);
      const content=document.createElement('div');row.append(content);
      const release=(data.releases||[]).find(x=>x.publication===item.publication);
      fleetText(content,'strong',(release?.tag||'版本未知')+' / '+(release?.firmware_id||'').slice(0,8)+' · '+fleetPhase(item.phase));
      fleetText(content,'small',fleetTime(item.updated)+(item.reason&&item.reason!=='none'?' · '+item.reason:''));list.append(row);
    }
  }
}
function renderFleet(data) {
  fleetState=data;
  const devices=fleetDevices(data);
  const now=Date.now()/1000;
  $('metric-devices').textContent=devices.length;
  $('metric-recent').textContent=devices.filter(d=>d.seen&&now-d.seen<=300).length;
  $('metric-success').textContent=(data.device_history||[]).filter(d=>d.phase==='success').length;
  $('metric-attention').textContent=(data.device_history||[]).filter(d=>d.category==='device-error'||d.category==='upgrade-failure').length;
  const current=(data.releases||[]).find(r=>r.publication===data.latest);
  $('dashboard-version').textContent=current?.tag||'尚未发布固件';
  $('dashboard-release-meta').textContent=current?
    `${FirmwareHardware.product(current.hwid)} · HWID ${current.hwid} · ID ${(current.firmware_id||'').slice(0,8)} · ${fleetTime(current.created)}`:
    '上传并发布固件后，设备才能获取更新。';
  $('dashboard-publication-status').textContent=current?`${(data.channels||[]).length} 个 HWID 通道`:'无发布';
  $('dashboard-publication-status').className='pill '+(current?'good':'warn');
  const activity=$('dashboard-activity');activity.replaceChildren();
  const recent=[...(data.device_history||[])].sort((a,b)=>b.updated-a.updated).slice(0,4);
  if(!recent.length)fleetText(activity,'li','暂无升级记录','muted');
  for(const item of recent){
    const li=document.createElement('li');activity.append(li);
    const dot=document.createElement('span');dot.className='dot '+fleetTone(item.phase);li.append(dot);
    const body=document.createElement('div');li.append(body);
    fleetText(body,'strong',item.device+' · '+fleetPhase(item.phase));
    const release=(data.releases||[]).find(r=>r.publication===item.publication);
    fleetText(body,'small',(release?.tag||'版本未知')+' / '+(release?.firmware_id||'').slice(0,8)+' · '+fleetTime(item.updated));
  }
  const overview=$('dashboard-devices');overview.replaceChildren();
  for(const item of devices.slice(0,5)){
    const row=document.createElement('tr');overview.append(row);
    fleetText(row,'td',item.device,'device-id');fleetText(row,'td',FirmwareHardware.product(item.hwid));
    fleetPill(fleetText(row,'td',''),fleetPhase(item.phase),fleetTone(item.phase));
    fleetText(row,'td',(item.release?.tag||'未知')+' / '+(item.release?.firmware_id||'').slice(0,8));
    fleetText(row,'td',fleetTime(item.seen));
  }
  if(!devices.length){const row=document.createElement('tr');overview.append(row);const td=fleetText(row,'td','暂无设备上报');td.colSpan=5;td.className='empty';}
  $('device-total').textContent=devices.length+' 台设备';
  const list=$('device-list');list.replaceChildren();
  const query=$('device-search').value.trim().toLowerCase();let shown=0;
  for(const item of devices){
    const searchable=JSON.stringify(item).toLowerCase();
    if(query&&!query.split(/\s+/).every(token=>searchable.includes(token)))continue;
    shown++;
    const row=document.createElement('tr');list.append(row);
    fleetText(row,'td',item.device,'device-id');
    const network=fleetText(row,'td',item.network?.ip||'未上报');
    if(item.network?.hostname)fleetText(network,'small',item.network.hostname);
    fleetText(row,'td',FirmwareHardware.product(item.hwid));
    const firmware=fleetText(row,'td',(item.release?.tag||'未知')+' / '+(item.release?.firmware_id||'').slice(0,8));
    firmware.title=item.release?.firmware_id||'';
    fleetPill(fleetText(row,'td',''),fleetPhase(item.phase),fleetTone(item.phase));
    fleetText(row,'td',fleetTime(item.seen));
    const action=fleetText(row,'td','');const button=document.createElement('button');
    button.className='secondary small';button.textContent='设备详情';button.onclick=()=>{selectedDevice=item.device;renderDeviceDetail(item,data);};action.append(button);
  }
  $('device-count').textContent=shown+' / '+devices.length+' 台';
  $('device-empty').hidden=shown>0;
  if(selectedDevice)renderDeviceDetail(devices.find(d=>d.device===selectedDevice),data);
}
function refreshFleetFromSol(){if(fleetState)renderFleet(fleetState);}
$('device-search').addEventListener('input',()=>{if(fleetState)renderFleet(fleetState);});
