const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const {runInNewContext}=require('node:vm');
const {test}=require('node:test');
const assert=require('node:assert/strict');
const source=readFileSync(join(__dirname,'../static/app.js'),'utf8');
const supportsClassifier=runInNewContext(source.slice(source.indexOf('function supportsClassifier('),source.indexOf('const classifierDrafts='))+';supportsClassifier');
test('classifier eligibility excludes switches, divert, VIA and unattached tunnels',()=>{
 assert.equal(supportsClassifier({kind:'adapter'}),true);
 assert.equal(supportsClassifier({kind:'tunnel',switch_socket:'/sw'}),true);
 assert.equal(supportsClassifier({kind:'tunnel',source:'discovered',metrics:{switch_connected:'1'}}),true);
 for(const e of [{kind:'switch'},{kind:'divert'},{kind:'tunnel'},{kind:'tunnel',switch_socket:'/sw',metrics:{relay_mode:'listen'}},{kind:'tunnel',source:'discovered',metrics:{switch_connected:'0'}}])assert.equal(supportsClassifier(e),false);
});
function actions({fail=false,confirm=true}={}) {
 const draft={text:'format 1\nclassify to [17]\n',active:'old',revision:'r',generation:'3',checked:{text:'format 1\nclassify to [17]\n',revision:'r',generation:'3'}};
 const calls=[],e={id:'control:a',kind:'adapter',name:'ex0'};
 const classifierAction=runInNewContext(source.slice(source.indexOf('async function classifierAction('),source.indexOf("$('classifier-siblings').addEventListener"))+';classifierAction',{
 selected:()=>e,supportsClassifier,classifierTargets:()=>[e],classifierBusy:false,classifierDraft:()=>draft,state:{data:{allow_write:true}},window:{confirm:()=>confirm},t:k=>k,renderClassifier(){},refresh:async()=>{},api:async(...args)=>{calls.push(args);if(fail)throw Error('lost response');return {result:'classifier_generation=4'};}
 });
 return {classifierAction,draft,calls};
}
test('load-flush sends generation once and invalidates the draft revision even on ambiguous failure',async()=>{
 for(const fail of [false,true]) {
  const {classifierAction,draft,calls}=actions({fail});await classifierAction('load-flush');
  assert.equal(calls.length,1);assert.equal(calls[0][2].expected_generation,'3');
  assert.equal(draft.revision,null);assert.equal(draft.checked,null);
 }
});
test('disable has no rules payload and cancel sends nothing',async()=>{
 const a=actions();await a.classifierAction('disable');assert.equal(a.calls[0][2].rules,'');
 const b=actions({confirm:false});await b.classifierAction('disable');assert.equal(b.calls.length,0);
});

const classifierBatch=runInNewContext(source.slice(source.indexOf('async function classifierBatch('),source.indexOf('async function classifierGroupAction('))+';classifierBatch');
const targets=[{id:'a',name:'a'},{id:'b',name:'b'},{id:'c',name:'c'}];
const review=targets.map(e=>({...e,result:{sha256:e.id,generation:'1'}}));
test('batch checks every target before first write and uses each generation',async()=>{
 const calls=[];
 const outcome=await classifierBatch(targets,'load','candidate',review,async(id,op,text,expected)=>{
  calls.push([id,op]);if(op==='check')return {sha256:id,generation:'1',diff:id};
  assert.equal(expected.sha256,id);return {result:'ok'};
 },()=>{});
 assert.equal(outcome.ok,true);
 assert.deepEqual(calls.map(c=>c[1]),['check','check','check','load','load','load']);
});
test('one unavailable or changed sibling prevents every write',async()=>{
 for(const failure of ['unavailable','changed']){
  const calls=[];
  const outcome=await classifierBatch(targets,'load-flush','candidate',review,async(id,op)=>{
   calls.push(op);if(id==='b' && failure==='unavailable')throw Error('offline');
   return {sha256:id,generation:id==='b'?'2':'1'};
  },()=>{});
  assert.equal(outcome.ok,false);assert.deepEqual(calls,['check','check','check']);
 }
});
test('ambiguous partial write stops without retry or rollback and preserves per-target result',async()=>{
 const calls=[];
 const outcome=await classifierBatch(targets,'load','candidate',review,async(id,op)=>{
  calls.push([id,op]);if(op==='check')return {sha256:id,generation:'1'};
  if(id==='b')throw Error('timeout; outcome unknown');return {result:'generation=2'};
 },()=>{});
 assert.equal(outcome.ok,false);
 assert.deepEqual(Array.from(outcome.results,r=>r.state),['applied','error','pending']);
 assert.equal(calls.filter(c=>c[1]==='load').length,2);
});
