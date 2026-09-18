const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const {runInNewContext}=require('node:vm');
const {test}=require('node:test');
const assert=require('node:assert/strict');
const source=readFileSync(join(__dirname,'../static/app.js'),'utf8');
const tunnelPayload=runInNewContext(source.slice(source.indexOf('function tunnelPayload('),source.indexOf('function payloadClass('))+';tunnelPayload');
const groups=runInNewContext(source.slice(source.indexOf('function observedGroups('),source.indexOf('const mapPinned='))+';observedGroups',{tunnelPayload});
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
