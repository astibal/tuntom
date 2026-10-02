const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const {runInNewContext}=require('node:vm');
const {test}=require('node:test');
const assert=require('node:assert/strict');
const source=readFileSync(join(__dirname,'../static/app.js'),'utf8');
const t=runInNewContext(source.slice(0,source.indexOf('function diagnostic('))+';t');
function harness(rows, endpoints){
 const fields=new Map();
 const $=id=>{if(!fields.has(id))fields.set(id,{value:'',checked:false,hidden:false,disabled:false,required:false,dataset:{},options:[{},{}],setCustomValidity(value){this.validationMessage=value;}});return fields.get(id);};
 const context={$,t,esc:String,state:{data:{endpoints}},supportedControlledEndpoints:()=>rows};
 const functions=source.slice(source.indexOf('function deploymentSwitches('),source.indexOf('function openDeploymentForm('))+
 source.slice(source.indexOf('function deploymentSide('),source.indexOf("$('deployment-form').addEventListener('submit'"));
 const api=runInNewContext(functions+';({deploymentSwitches,canCreateDeployment,updateDeploymentForm,deploymentSide})',context);
 return {...api,$};
}
test('one host can connect to local observed switch; routed switch cannot host local process',()=>{
 const h=harness([{id:'a'}],[{id:'s',kind:'switch',source:'local',switch_socket:'/run/core.sock'},{id:'r',kind:'switch',source:'discovered',switch_socket:'/run/remote.sock'}]);
 assert.equal(h.canCreateDeployment(),true);
 assert.equal(h.deploymentSwitches().length,1);
 assert.equal(harness([{id:'a'}],[]).canCreateDeployment(),false);
 assert.equal(harness([{id:'a'},{id:'b'}],[]).canCreateDeployment(),true);
});
test('switch mode hides SSH target and sends observed identity without socket path',()=>{
 const h=harness([],[]),$=h.$;
 $('deployment-mode').value='switch';$('deployment-initiator').value='a';$('deployment-existing-switch').value='observed-switch';
 $('deployment-b-port').value='edge';$('deployment-b-label').value='17';$('deployment-b-socket').value='/user/supplied';
 h.updateDeploymentForm();
 assert.equal($('deployment-b-endpoint').disabled,true);
 assert.equal($('deployment-b-endpoint').required,false);
 assert.equal($('deployment-b-endpoint-wrap').hidden,true);
 assert.equal($('deployment-a-peer').required,true);
 const side=h.deploymentSide('b','listener');
 assert.equal(side.switch_id,'observed-switch');assert.equal(side.attachment.type,'switch');
 assert.equal(side.endpoint_id,undefined);assert.equal(side.attachment.switch_socket,undefined);
 $('deployment-mode').value='ssh';h.updateDeploymentForm();
 assert.equal($('deployment-b-endpoint').disabled,false);
 assert.equal($('deployment-existing-switch-wrap').hidden,true);
});
test('initiator names and default IP follow selected hosts without overwriting manual transport IP',()=>{
 const h=harness([{id:'a',address:'192.0.2.1',name:'vpn'},{id:'b',address:'192.0.2.2',name:'core'}],[]),$=h.$;
 $('deployment-a-endpoint').value='a';$('deployment-b-endpoint').value='b';$('deployment-mode').value='ssh';$('deployment-initiator').value='a';
 h.updateDeploymentForm();
 assert.equal($('deployment-initiator').options[0].textContent,'vpn');assert.equal($('deployment-initiator').options[1].textContent,'core');
 assert.equal($('deployment-a-peer').value,'192.0.2.2');assert.equal($('deployment-b-peer').value,'192.0.2.1');
 $('deployment-a-peer').value='198.51.100.2';h.updateDeploymentForm();assert.equal($('deployment-a-peer').value,'198.51.100.2');
});
test('occupied switch port suffix is rejected and auto ID does not need a sample value',()=>{
 const h=harness([],[{id:'s',kind:'switch',source:'local',switch_socket:'/run/sw',switch_detail:{ports:[{name:'edge_2'}]}}]),$=h.$;
 $('deployment-mode').value='switch';$('deployment-existing-switch').value='s';$('deployment-b-port').value='edge';$('deployment-count').value='4';$('deployment-auto-id').checked=true;
 h.updateDeploymentForm();assert.match($('deployment-b-port').validationMessage,/edge_2/);assert.equal($('deployment-tunnel-id').required,false);
 $('deployment-auto-id').checked=false;h.updateDeploymentForm();assert.equal($('deployment-tunnel-id').required,true);
});
test('endpoint shows deployment state, IDs and undeploy; archives precede loose files',()=>{
 const row={id:'d',name:'vpn',status:'running',updated_at:'2026-09-30T00:00:00Z',config:{tunnel_id:1,count:2,id_allocation:'automatic',side_a:{endpoint_id:'a',role:'initiator'},side_b:{endpoint_id:'b',role:'listener'}},scripts:['side_a/scripts/95-undeploy.sh']};
 const target={innerHTML:''};
 const helpers=source.slice(source.indexOf('function deploymentStatus('),source.indexOf('async function undeployFromUI('));
 const render=source.slice(source.indexOf('function renderDeployments('),source.indexOf("$('deployment-new').addEventListener"));
 const api=runInNewContext(helpers+render+';({endpointTunnelInfo,renderDeployments,canUndeploy})',{$:()=>target,t,esc:String,locale:()=> 'cs-CZ',state:{auth:{role:'admin'}},controlledView:{rows:[],deployments:[row]}});
 assert.match(api.endpointTunnelInfo('a'),/Nasazeno/);assert.match(api.endpointTunnelInfo('a'),/1, 1_1/);
 assert.match(api.endpointTunnelInfo('a'),/Undeploy/);api.renderDeployments();
 assert.ok(target.innerHTML.indexOf('deploy_runbook_vpn.tar.gz')<target.innerHTML.indexOf('undeploy_runbook_vpn.tar.gz'));
 assert.ok(target.innerHTML.indexOf('undeploy_runbook_vpn.tar.gz')<target.innerHTML.indexOf('95-undeploy.sh'));
 row.status='undeployed';assert.equal(api.canUndeploy(row),false);
});
