// Isolated headless Chrome; it never opens the user's browser profile.
import {spawn} from 'node:child_process';
import {readFile, writeFile, mkdir, rm} from 'node:fs/promises';
import {resolve} from 'node:path';
const root=resolve(import.meta.dirname,'..');
const profile=resolve(root,'.tools/preview-chrome');
await mkdir(profile,{recursive:true});
await rm(profile+'/DevToolsActivePort',{force:true});
await mkdir(resolve(root,'artifacts'),{recursive:true});
const chrome=spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',[
 '--headless','--disable-gpu','--no-first-run','--no-default-browser-check','--remote-debugging-port=0',
 '--user-data-dir='+profile,'about:blank'],{stdio:'ignore'});
const pause=ms=>new Promise(r=>setTimeout(r,ms));
let socket;
try {
 let port;
 for(let i=0;i<50;i++){try{port=Number((await readFile(profile+'/DevToolsActivePort','utf8')).split('\n')[0]);if(port)break}catch{}await pause(100)}
 if(!port)throw Error('Chrome did not start');
 const pages=await (await fetch('http://127.0.0.1:'+port+'/json/list')).json();
 socket=new WebSocket(pages[0].webSocketDebuggerUrl);
 await new Promise((resolve,reject)=>{socket.addEventListener('open',resolve,{once:true});socket.addEventListener('error',reject,{once:true})});
 let sequence=0;const pending=new Map();
 socket.addEventListener('message',e=>{const r=JSON.parse(e.data);if(pending.has(r.id)){const {resolve,reject}=pending.get(r.id);pending.delete(r.id);r.error?reject(Error(r.error.message)):resolve(r.result)}});
 const call=(method,params={})=>new Promise((resolve,reject)=>{const id=++sequence;pending.set(id,{resolve,reject});socket.send(JSON.stringify({id,method,params}))});
 const evaluate=async expression=>{const r=await call('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(r.exceptionDetails)throw Error('Preview expression failed: '+r.exceptionDetails.text+' '+(r.exceptionDetails.exception?.className||''));return r.result.value};
 await call('Emulation.setDeviceMetricsOverride',{width:720,height:840,deviceScaleFactor:1,mobile:false});
 await call('Page.navigate',{url:'http://127.0.0.1:8787'});
 for(let i=0;i<50;i++){if(await evaluate('!!document.querySelector("#connect") && typeof render==="function"'))break;await pause(100)}
 const token=(await readFile(resolve(root,'bridge/secrets/device-token'),'utf8')).trim();
 await evaluate(`document.querySelector('#token').value=${JSON.stringify(token)};document.querySelector('#connect').click();true`);
 for(let i=0;i<50;i++){if(await evaluate('typeof data!=="undefined" && !!data'))break;await pause(100)}
 if(!(await evaluate('!!data && data.schema_version===1')))throw Error('Live snapshot was not loaded');
 await evaluate('document.querySelector("#token").value="";true');
 for(const [index,name] of ['overview','quota','today'].entries()){
   if(!(await evaluate(`(()=>{const r=canvas.getBoundingClientRect();canvas.dispatchEvent(new MouseEvent('click',{clientX:r.left+(35+${index}*107)*r.width/320,clientY:r.top+220*r.height/240}));return view===${index}})()`)))throw Error('Preview tab did not switch: '+name);
   const screenshot=await call('Page.captureScreenshot',{format:'png'});
   await writeFile(resolve(root,'artifacts/preview-'+name+'.png'),Buffer.from(screenshot.data,'base64'));
 }
 const summary=await evaluate('({accounts:data.summary.accounts_total,partial:data.partial,page:data.pagination.page})');
 console.log('Preview passed: '+JSON.stringify(summary)+'; captured 3 screens.');
} finally {
 if(socket)socket.close();chrome.kill('SIGTERM');
}
