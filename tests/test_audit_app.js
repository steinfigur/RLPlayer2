// Actual executables and HTTP handlers, isolated application directory, null audio backend only.
const fs=require('fs'),path=require('path'),http=require('http'),net=require('net'),cp=require('child_process');
const [build,base]=process.argv.slice(2);if(!build||!base)throw new Error('build and isolated root required');
fs.mkdirSync(base,{recursive:true});const root=fs.mkdtempSync(path.join(base,'run-'));let checks=0,child,port;
const check=(v,m)=>{checks++;if(!v)throw new Error(m);};
const pause=ms=>new Promise(r=>setTimeout(r,ms));
function wave(file,rate,frames){const b=Buffer.alloc(44+frames*4);b.write('RIFF');b.writeUInt32LE(b.length-8,4);b.write('WAVEfmt ',8);b.writeUInt32LE(16,16);b.writeUInt16LE(1,20);b.writeUInt16LE(2,22);b.writeUInt32LE(rate,24);b.writeUInt32LE(rate*4,28);b.writeUInt16LE(4,32);b.writeUInt16LE(16,34);b.write('data',36);b.writeUInt32LE(frames*4,40);for(let i=44;i<b.length;i+=2)b.writeInt16LE(4096,i);fs.writeFileSync(file,b);}
const env={...process.env,PATH:path.resolve(build,'../ffmpeg')+';'+process.env.PATH,RLPLAYER_LIBRARY_DIR:path.join(root,'library'),RLPLAYER_UPDATE_DIR:root,TEMP:path.join(root,'tmp'),TMP:path.join(root,'tmp')};fs.mkdirSync(env.TEMP,{recursive:true});
function request(route,body='',method='POST'){return new Promise((resolve,reject)=>{const b=Buffer.isBuffer(body)?body:Buffer.from(body);const r=http.request({host:'127.0.0.1',port,path:route,method,headers:{'Content-Length':b.length,'Content-Type':'application/x-www-form-urlencoded'},timeout:10000},res=>{let text='';res.on('data',b=>text+=b);res.on('end',()=>{try{resolve({status:res.statusCode,data:JSON.parse(text),text});}catch(e){reject(new Error(route+' returned invalid JSON: '+text.slice(0,100)));}});});r.on('error',reject);r.on('timeout',()=>r.destroy(new Error('timeout '+route)));r.end(b);});}
async function until(fn,ms=8000){let e;for(let t=Date.now();Date.now()-t<ms;){try{const r=await fn();if(r)return r;}catch(x){e=x;}await pause(40);}throw e||new Error('condition timed out');}
(async()=>{
 for(const f of ['rtaweb.exe','rtaplay.exe'])fs.copyFileSync(path.join(build,f),path.join(root,f));
 fs.writeFileSync(path.join(root,'rlplayer-settings.conf'),'backend=null\nvolume=-60\nmetaon=0\nmetallm=0\nmetasummary=0\nroomcorrauto=0\ngpuvendor=off\n');
 const music=path.join(root,'fixture.wav');wave(music,48000,21600);
 port=await new Promise(r=>{const s=net.createServer();s.listen(0,'127.0.0.1',()=>{const p=s.address().port;s.close(()=>r(p));});});
 child=cp.spawn(path.join(root,'rtaweb.exe'),['--port',String(port)],{cwd:root,env,windowsHide:true});
 let output='';child.stdout.on('data',b=>output+=b);child.stderr.on('data',b=>output+=b);
 await until(async()=>(await request('/api/ping','','GET')).data.ok);check(child.exitCode===null,'server started');
 for(const suffix of ['XYZ','%3f','&x=1']){const r=await request('/api/quit'+suffix,'','GET');check(r.data.ok!==true,'suffix cannot invoke quit');check(child.exitCode===null,'suffix preserved process');}
 for(const p of ['/api/quit','/api/stop','/api/lib/play','/api/lib/enqueue'])check((await request(p,'','GET')).status===405,'GET rejected '+p);
 for(const v of ['nan','inf','1e999','1junk',''])check((await request('/api/volume?db='+v)).status===400,'bad volume '+v);
 for(const body of ['preamp=nan','bands=1,PK,nan,0,1','bands=1,PK,1000,0,1junk'])check((await request('/api/peq',body)).status===400,'bad PEQ rejected');
 const before=fs.readFileSync(path.join(root,'rlplayer-settings.conf'));
 check((await request('/api/settings','volume=nan')).status===400,'settings refuses invalid numbers');check(fs.readFileSync(path.join(root,'rlplayer-settings.conf')).equals(before),'invalid settings preserve file');
 check((await request('/api/cast/start','{"rate":1e300}')).status===400,'JSON narrowing rejected');
 check((await request('/api/cast/start','seconds=nan')).status===400,'cast NaN rejected');
 const a=await request('/api/upload?name=01.wav',Buffer.from('A'));
 const b=await request('/api/upload?name=01.wav',Buffer.from('BB'));
 check(a.data.ok&&b.data.ok&&a.data.path!==b.data.path,'same basename gets distinct identity');
 check(fs.readFileSync(a.data.path).toString()==='A'&&fs.readFileSync(b.data.path).toString()==='BB','old upload bytes preserved');
 const form='file='+encodeURIComponent(music)+'&backend=null&volume=0&isp=0&roomcorrauto=0&gpuvendor=off&shaper=none&bits=16';
 for(const backend of ['naa','invalid-backend'])check((await request('/api/play',form+'&backend='+backend)).status===400,'unknown backend cannot fall through to a physical device');
 for(const extra of ['','&directsdm=1','&dsdnative=1'])check((await request('/api/play',form+'&dsd=64&modfamily=ahm'+extra)).status===400,'AHM capability rejected before mutation');
 check((await request('/api/play',form)).data.ok,'null playback accepted');
 await until(async()=>(await request('/api/status','','GET')).data.dspChunks>=7);
 check((await request('/api/pause?on=1')).data.ok,'pause accepted');
 await until(async()=>{const r=await request('/api/status','','GET');return r.data.paused||r.data.status?.paused;},2000);
 await pause(100);
 const chunksBeforeSeek=(await request('/api/status','','GET')).data.dspChunks;
 check((await request('/api/seek?pos=0.5')).data.ok,'seek accepted after queued EOF');
 await request('/api/pause?on=0');await pause(80);
 check(child.exitCode===null,'post-EOF seek does not terminate server');
 const afterSeek=await until(async()=>{const s=(await request('/api/status','','GET')).data;return !s.playing&&s.pos>=0.449?s:null;});
 check(afterSeek.dspChunks>chunksBeforeSeek&&!afterSeek.error,'post-EOF seek actually resumes production and renders to source end');await request('/api/stop');
 for(let i=0;i<3;i++){const r=await Promise.all([request('/api/play',form),request('/api/play',form),request('/api/stop'),request('/api/queue/set','files='+encodeURIComponent(music))]);check(r.every(x=>typeof x.data==='object'),'concurrent lifecycle returns JSON');check(child.exitCode===null,'concurrent lifecycle survives');}
 await request('/api/stop');
 check((await request('/api/peq','preamp=-12&bypass=0')).data.ok,'PEQ enabled');check((await request('/api/play',form)).data.ok,'PEQ playback');
 const signal=await until(async()=>{const r=await request('/api/signal','','GET');let d=r.data;if(d.path)d=d.path;if(d.signal)d=typeof d.signal==='string'?JSON.parse(d.signal):d.signal;return d.bitPerfect==='no'?d:null;});
 check(signal.bitPerfectWhy.includes('PEQ'),'signal reports actual PEQ');
 const status=(await request('/api/status','','GET')).data;check(status.bitPerfect==='no'&&status.bitPerfectWhy===signal.bitPerfectWhy,'status and signal integrity agree');
 await request('/api/stop');
 const cli=path.join(root,'rtaplay.exe'),cfg=path.join(root,'cli.conf'),saved=path.join(root,'saved.conf'),out=path.join(root,'quiet.wav');
 fs.writeFileSync(cfg,'volume=0\nfilter=poly-sinc-ext2\n');
 let r=cp.spawnSync(cli,[music,'--analyze','--out',out,'--config',cfg,'--save-config',saved,'--volume','-60','--taps','4096','--bits','16'],{cwd:root,env,encoding:'utf8',timeout:60000});fs.writeFileSync(path.join(root,'cli.log'),r.stdout+r.stderr);check(r.status===0,'offline CLI execution');check(/taps=4096/.test(fs.readFileSync(saved,'utf8')),'CLI explicit taps win over preset');
 const quiet=fs.readFileSync(out);let quietPeak=0;for(let i=44;i+1<quiet.length;i+=2)quietPeak=Math.max(quietPeak,Math.abs(quiet.readInt16LE(i)));check(quietPeak>0&&quietPeak<=8,'CLI explicit -60dB wins over configuration 0dB');
 const short=path.join(root,'32k.wav'),dop=path.join(root,'dop.wav');wave(short,32000,4096);
 r=cp.spawnSync(cli,[short,'--analyze','--out',dop,'--dsd','64','--taps','1024'],{cwd:root,env,encoding:'utf8',timeout:60000});fs.writeFileSync(path.join(root,'cli-dop.log'),r.stdout+r.stderr);check(r.status===0,'non-integer ratio DSD CLI completes');check(fs.statSync(dop).size>44,'DSD file published');
 const encoded=fs.readFileSync(dop);const dopFrames=(encoded.length-44)/6;check(dopFrames===Math.ceil(Math.ceil(4096*2822400/32000)/16),'partial DSD source bits get final carrier frame');
 let markers=true;for(let f=0;f<dopFrames;f++)for(let ch=0;ch<2;ch++)if(encoded[44+f*6+ch*3+2]!==((f%2)?250:5))markers=false;check(markers,'offline DoP markers alternate on every channel');
 await request('/api/quit');await until(()=>child.exitCode!==null,5000);check(child.exitCode===0,'graceful actual server shutdown');fs.writeFileSync(path.join(root,'server.log'),output);
 fs.writeFileSync(path.join(root,'result.json'),JSON.stringify({checks,root,devices:0,exit:0},null,2));console.log('[PASS] actual app checks='+checks+' devices=0 root='+root);
})().catch(async e=>{console.error(e.stack);if(child&&child.exitCode===null){try{await request('/api/quit');await pause(500);}catch{}if(child.exitCode===null)console.error('owned test server still running pid='+child.pid);}process.exitCode=1;});
