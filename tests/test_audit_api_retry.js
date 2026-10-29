// Run the production request wrapper with real JavaScript Promises, no server.
const fs = require('fs');
const vm = require('vm');
const source = fs.readFileSync(require('path').join(__dirname, '../src/net/ui_script.h'), 'utf8');
const begin = source.indexOf('function api(u, body) {');
const end = source.indexOf('\nfunction renderPanelError', begin);
if (begin < 0 || end < 0) throw new Error('production API wrapper not found');
(async () => {
  let checks = 0;
  for (const failure of [new Error('response lost after mutation'), Object.assign(new Error('HTTP 500'), {status:500}), Object.assign(new Error('HTTP 409'), {status:409})]) {
    let mutations = 0;
    const api = vm.runInNewContext(source.slice(begin, end) + '\napi', {apiRaw: async () => { mutations++; throw failure; }});
    let actual;
    try { await api('/api/skip?dir=1'); } catch (e) { actual=e; }
    if (mutations !== 1 || actual !== failure) throw new Error('ambiguous POST was replayed or hidden');
    checks++;
  }
  let calls=0;
  const api=vm.runInNewContext(source.slice(begin,end)+'\napi',{apiRaw:async()=>{calls++;return {ok:true};}});
  if (!(await api('/api/play','file=fixture')).ok || calls!==1) throw new Error('success response changed');
  console.log('[PASS] production API retry checks='+(checks+1));
})().catch(e=>{console.error(e);process.exit(1);});
