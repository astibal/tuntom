const {test}=require('node:test');
const assert=require('node:assert/strict');
const {readFileSync}=require('node:fs');
const {runInNewContext}=require('node:vm');
const source=readFileSync(require('node:path').join(__dirname,'../static/app.js'),'utf8');
const section=source.slice(source.indexOf('function parseDiscovery('),source.indexOf('function renderDiagnostics('));
function fixture(api) {
 const nodes={};let current={id:'a',control:'/socket'};
 const context={state:{discoveries:new Map()},selected:()=>current,$:id=>nodes[id]??=( {} ),
  t:key=>key,diagnostic:value=>value,locale:()=> 'en',esc:value=>String(value).replaceAll('<','&lt;'),
  api,setTimeout:fn=>fn(),Date};
 const functions=runInNewContext(section+';({parseDiscovery,renderDiscovery,discoverNetwork})',context);
 return {...functions,context,nodes,select:e=>current=e};
}
const header='path\tstate\tinstance\tcomponent\tcapabilities\n';
const response=header+'self\tFOUND\t'+'a'.repeat(32)+'\tswitch\tcontrol,discover\n';
test('discovery validates schema, preserves alternate paths and distinguishes no response',()=>{
 const {parseDiscovery}=fixture();
 assert.equal(parseDiscovery(response)[0].instance,'a'.repeat(32));
 assert.equal(parseDiscovery(header+'peer\tNO_RESPONSE\t-\t-\t-\n')[0].state,'NO_RESPONSE');
 assert.equal(parseDiscovery(response.replace('FOUND','ALT_PATH'))[0].state,'ALT_PATH');
 for(const bad of ['oops',header+'bad',response.replace('FOUND','UNKNOWN'),response.replace('a'.repeat(32),'bad')])assert.throws(()=>parseDiscovery(bad));
});
test('submit once then poll; result remains scoped to origin after selection changes',async()=>{
 let calls=[];let f;
 f=fixture(async(path,method)=>{
  calls.push([path,method]);
  if(method==='POST'){f.select({id:'b',control:'/b'});return {id:'job',state:'running'};}
  return {id:'job',state:'succeeded',result:{text:response}};
 });
 await f.discoverNetwork();
 assert.equal(calls.length,2);assert.equal(calls[0][1],'POST');
 assert.equal(f.context.state.discoveries.get('a').rows.length,1);
 assert.equal(f.context.state.discoveries.has('b'),false);
 assert.equal(f.nodes['discovery-results'].innerHTML,'');
 f.select({id:'a',control:'/a'});f.renderDiscovery();
 assert.match(f.nodes['discovery-results'].innerHTML,/FOUND/);
});
test('failure unlocks action and does not resubmit; remote text is escaped',async()=>{
 let count=0;const f=fixture(async()=>{count++;throw new Error('unsupported');});
 await f.discoverNetwork();assert.equal(count,1);
 assert.equal(f.context.state.discoveries.get('a').busy,false);
 assert.equal(f.nodes['discovery-read'].disabled,false);
 f.context.state.discoveries.set('a',{rows:[{path:'<img>',state:'FOUND',instance:'a',component:'x',capabilities:'x'}],time:Date.now()});
 f.renderDiscovery();assert.doesNotMatch(f.nodes['discovery-results'].innerHTML,/<img>/);
});
