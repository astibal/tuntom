const {test}=require('node:test');
const assert=require('node:assert/strict');
const {readFileSync}=require('node:fs');
const {runInNewContext}=require('node:vm');
const source=readFileSync(require('node:path').join(__dirname,'../static/app.js'),'utf8');
const section=source.slice(source.indexOf('function controlState('),source.indexOf('function renderDiagnostics('));
const capable={status:'reachable',metrics:{control_enabled:'1',control_discover_enabled:'1',control_can_initiate:'1'}};
function fixture(api) {
 const nodes={};let current={id:'a',control:'/socket',...capable};
 const context={state:{discoveries:new Map()},selected:()=>current,$:id=>nodes[id]??=( {} ),
  t:key=>key,diagnostic:value=>value,locale:()=> 'en',esc:value=>String(value).replaceAll('<','&lt;'),
  api,setTimeout:fn=>fn(),Date};
 const functions=runInNewContext(section+';({parseDiscovery,renderDiscovery,discoverNetwork,controlState,authorityBadge})',context);
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
 f.select({id:'a',control:'/a',...capable});f.renderDiscovery();
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

test('CONTROL stats gate discovery, including trusted receivers and older nodes',async()=>{
 let requests=0;const f=fixture(async()=>{requests++;});
 const endpoint={id:'a',control:'/a',...capable};
 assert.equal(f.controlState(endpoint).canDiscover,true);
 for(const fields of [{control_enabled:'0'},{control_discover_enabled:'0'},{control_can_initiate:'0'},
                      {control_can_initiate:undefined},{control_enabled:'unknown'}]) {
   const e={...endpoint,metrics:{...endpoint.metrics,...fields}};
   assert.equal(f.controlState(e).canDiscover,false);
   f.select(e);f.renderDiscovery();assert.equal(f.nodes['discovery-read'].hidden,true);
   await f.discoverNetwork();
 }
 for(const e of [{...endpoint,metrics:{}},{...endpoint,status:'unavailable'},{...endpoint,control:''}]) {
   assert.equal(f.controlState(e).canDiscover,false);
 }
 assert.equal(requests,0);
 f.select(endpoint);f.renderDiscovery();assert.equal(f.nodes['discovery-read'].hidden,false);
});
test('authority badge uses loaded public authority keys, not trusted keys or debug mode',()=>{
 const f=fixture(),e={id:'a',control:'/a',...capable};
 const key='a'.repeat(64);
 for(const value of ['NONE',undefined,'bad','<img>'])assert.equal(f.controlState({...e,metrics:{...e.metrics,control_authority_keys:value}}).authority,false);
 assert.equal(f.controlState({...e,metrics:{...e.metrics,control_trusted_keys:key,control_access:'allow-all'}}).authority,false);
 const authority={...e,metrics:{...e.metrics,control_authority_keys:key+','+'b'.repeat(64),control_enabled:'0'}};
 assert.equal(f.controlState(authority).authority,true);
 assert.equal(f.controlState(authority).canDiscover,false);
 assert.match(f.authorityBadge(authority),/authority-badge/);
 assert.equal(f.authorityBadge({...authority,status:'unavailable'}),'');
});
test('CONTROL details preserve uint64 masks and levels and never render unescaped values',()=>{
 const f=fixture();f.select({id:'a',...capable,metrics:{control_required_caps:'18446744073709551615',control_required_level:'18446744073709551614',control_access:'<img>'}});
 f.renderDiscovery();const html=f.nodes['control-settings-values'].innerHTML;
 assert.match(html,/18446744073709551615/);assert.match(html,/18446744073709551614/);
 assert.doesNotMatch(html,/<img>/);assert.match(html,/control_forward_enabled/);
});
test('remote component marker distinguishes discovered items and does not invent a PID',()=>{
 const body=source.slice(source.indexOf('function payloadMark('),source.indexOf('function endpointType('));
 const f=runInNewContext(body+';({payloadMark,processIdentity})',{esc:x=>String(x),t:x=>x,tunnelPayload:()=>null});
 const remote={source:'discovered',kind:'adapter',pid:null};
 assert.match(f.payloadMark(remote),/discovered-badge/);
 assert.equal(f.processIdentity(remote),'CONTROL');
 assert.doesNotMatch(f.payloadMark({...remote,source:'local',pid:42}),/discovered-badge/);
 assert.match(f.payloadMark({...remote,discovery_stale:true}),/discoveryStale/);
});
