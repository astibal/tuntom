const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const {runInNewContext}=require('node:vm');
const {test}=require('node:test');
const assert=require('node:assert/strict');
const source=readFileSync(join(__dirname,'../static/app.js'),'utf8');
const tunnelPayload=runInNewContext(source.slice(source.indexOf('function tunnelPayload('),source.indexOf('function payloadClass('))+';tunnelPayload');
const controlTopology=runInNewContext(source.slice(source.indexOf('function controlTopology('),source.indexOf('function observedPlacement('))+';controlTopology');
const groups=runInNewContext(source.slice(source.indexOf('function observedGroups('),source.indexOf('const mapPinned='))+';observedGroups',{tunnelPayload,controlTopology});
const endpoint=(member=0,other={})=>({id:'e'+member,kind:'tunnel',name:'231'+(member?'_'+member:'')+'s',host:'h',net_namespace:'net1',mount_namespace:'mnt1',role:'server',status:'reachable',switch_socket:'/sw',peer:'',metrics:{tunnel_id:String(231+256*member)},...other});
test('map stacks verify encoded IDs, roles, payload types and local context',()=>{
 assert.equal(groups([endpoint(2),endpoint(0),endpoint(1)]).length,1);
 assert.deepEqual(Array.from(groups([endpoint(2),endpoint(0)])[0].members,e=>e.id),['e0','e2']);
 for(const override of [{metrics:{tunnel_id:'231'}},{role:'client'},{net_namespace:'net2'},{mount_namespace:''},{switch_socket:'/other'},{peer:'192.0.2.1'},
   {metrics:{tunnel_id:'487',relay_mode:'connect'}},{status:'unavailable'}]){
  assert.equal(groups([endpoint(0),endpoint(1,override)]).length,2,JSON.stringify(override));
 }
});
test('singletons and similarly named non-tunnels are not merged',()=>{
 assert.equal(groups([endpoint(0),endpoint(1,{kind:'adapter'})]).length,2);
 assert.equal(groups([endpoint(0,{name:'999s'}),endpoint(1,{name:'999_1s'})]).length,2);
});

const localLinks=[0,1].map(i=>({source:'e'+i,target:'sw',port_id:'edge'+i,namespace_verified:true}));
const discovered=(i,extra={})=>({id:'r'+i,kind:'tunnel',name:'unrelated-'+i,source:'discovered',status:'reachable',metrics:{tunnel_id:String(231+256*i)},control_routes:[{origin:'sw',path:'port:edge'+i+'/peer'}],...extra});
const bundleNodes=()=>[{id:'sw',kind:'switch'},endpoint(0),endpoint(1),discovered(0),discovered(1)];
test('discovered peers bundle only through verified local stack attachments, independent of names',()=>{
 const result=groups(bundleNodes(),localLinks),bundle=result.find(g=>g.key.startsWith('peers:'));
 assert.equal(bundle.name,'231s / peer');
 assert.deepEqual(Array.from(bundle.members,e=>e.id),['r0','r1']);
 assert.equal(result.flatMap(g=>Array.from(g.members)).length,5);
 const reversed=groups(bundleNodes().reverse(),localLinks).find(g=>g.key===bundle.key);
 assert.deepEqual(Array.from(reversed.members,e=>e.id),['r0','r1']);
});
test('unknown attachments, stale discovery, mixed payload and deeper descendants are not merged',()=>{
 assert.equal(groups(bundleNodes(),localLinks.map(l=>({...l,namespace_verified:false}))).filter(g=>g.members[0].source==='discovered').length,2);
 for(const extra of [{discovery_stale:true},{metrics:{relay_mode:'connect'}},{control_routes:[{origin:'sw',path:'port:edge1/peer/port:next'}]}]){
  const nodes=bundleNodes();nodes[4]=discovered(1,extra);
  assert.equal(groups(nodes,localLinks).filter(g=>g.members[0].source==='discovered').length,2);
 }
});

const adapter=(i,extra={})=>({id:'a'+i,source:'discovered',kind:'divert',name:'proxy-'+i,control_routes:[{origin:'sw',path:'port:edge'+i+'/peer/port:proxy'}],...extra});
test('adapter bundles follow direct attachments to a verified peer stack',()=>{
 const nodes=[...bundleNodes(),adapter(0),adapter(1)];
 const result=groups(nodes,localLinks),bundle=result.find(g=>g.key.startsWith('adapters:'));
 assert.deepEqual(Array.from(bundle.members,e=>e.id),['a0','a1']);
 assert.equal(result.flatMap(g=>Array.from(g.members)).length,nodes.length);
 assert.deepEqual(Array.from(groups([...nodes].reverse(),localLinks).find(g=>g.key===bundle.key).members,e=>e.id),['a0','a1']);
});
test('adapter bundles exclude ambiguous parents, extra siblings, stale paths and mixed kinds',()=>{
 for(const extra of [{discovery_stale:true},{kind:'adapter'},{control_routes:[{origin:'sw',path:'port:edge1/peer/port:missing/port:proxy'}]},
 {control_routes:[{origin:'sw',path:'port:edge0/peer/port:other'},{origin:'sw',path:'port:edge1/peer/port:proxy'}]}]){
  const result=groups([...bundleNodes(),adapter(0),adapter(1,extra)],localLinks);
  assert.ok(!result.some(g=>g.key.startsWith('adapters:') && g.members.length>1));
 }
 const result=groups([...bundleNodes(),adapter(0),adapter(1),adapter(0,{id:'extra',control_routes:[{origin:'sw',path:'port:edge0/peer/port:other'}]})],localLinks);
 assert.ok(!result.some(g=>g.key.startsWith('adapters:') && g.members.length>1));
});

const processBundles=runInNewContext(source.slice(source.indexOf('function processBundles('),source.indexOf('function supportsProcessView('))+';processBundles',{observedGroups:groups});
test('table filters retain verified grouping from the complete topology without losing or adding rows',()=>{
 const nodes=[...bundleNodes(),adapter(0),adapter(1)];
 const filtered=nodes.filter(e=>e.kind==='divert');
 const result=processBundles(nodes,localLinks,filtered);
 assert.equal(result.length,1);
 assert.deepEqual(Array.from(result[0].members,e=>e.id),['a0','a1']);
 const one=processBundles(nodes,localLinks,[nodes.find(e=>e.id==='a1')]);
 assert.equal(one.length,1);assert.equal(one[0].members.length,1);
 assert.equal(processBundles(nodes,localLinks,[]).length,0);
});

const labelPortGroups=runInNewContext(source.slice(source.indexOf('function labelPortGroups('),source.indexOf('function renderLabelTopology('))+';labelPortGroups',{observedGroups:groups});
test('label topology presents verified sibling attachments as one logical port group',()=>{
 const result=labelPortGroups(bundleNodes(),localLinks,'sw',[{name:'edge0'},{name:'edge1'},{name:'admin'}]);
 const bundle=result.find(group=>group.name==='231s');
 assert.deepEqual(Array.from(bundle.ports),['edge0','edge1']);
 assert.equal(result.find(group=>group.name==='admin').ports.length,1);
 assert.equal(result.length,2);
});
