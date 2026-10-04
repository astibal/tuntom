const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const {runInNewContext}=require('node:vm');
const {test}=require('node:test');
const assert=require('node:assert/strict');
const source=readFileSync(join(__dirname,'../static/app.js'),'utf8');
const now=Date.parse('2026-10-02T12:00:00Z');
const at=seconds=>new Date(now-seconds*1000).toISOString();
const state={data:{poll_interval_seconds:5,network_discovery:{interval_seconds:120}},failure:''};
const {endpointFreshness,mapFreshness}=runInNewContext(source.slice(source.indexOf('function endpointFreshness('),source.indexOf('function warningChecks('))+';({endpointFreshness,mapFreshness})',{
 state,Date:class extends Date{static now(){return now;}},duration:n=>`${n}s`,diagnostic:String,
 t:(key,args={})=>key+JSON.stringify(args),esc:v=>String(v).replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('"','&quot;')
});
const fresh=()=>({name:'peer',status:'reachable',source:'discovered',sampled_at:at(2),last_success_at:at(2),discovered_at:at(100)});
test('fresh discovery and polling add no badge',()=>{
 assert.equal(mapFreshness([fresh()]).badge,'');
});
test('old discovery does not mark live polling unavailable',()=>{
 const e={...fresh(),discovered_at:at(241)};
 assert.equal(endpointFreshness(e).discoveryStale,true);
 assert.equal(endpointFreshness(e).pollStale,false);
 assert.equal(mapFreshness([e]).stale,false);
 assert.match(mapFreshness([e]).hint,/freshnessDiscovery/);
});
test('failed polling uses last successful timestamp, not recent failed attempt',()=>{
 const e={...fresh(),status:'unavailable',sampled_at:at(1),last_success_at:at(80)};
 assert.equal(endpointFreshness(e).pollAge,80);
 assert.equal(mapFreshness([e]).stale,true);
});
test('a hidden stale sibling is included and remote errors are escaped',()=>{
 const result=mapFreshness([fresh(),{...fresh(),name:'<bad>',last_success_at:at(40),error:'<img>'}]);
 assert.match(result.badge,/×1/);assert.match(result.badge,/&lt;img>/);assert.doesNotMatch(result.badge,/<img>/);
 assert.equal(result.stale,true);
});
test('local nodes need no discovery date; missing telemetry dates are unknown',()=>{
 assert.equal(mapFreshness([{...fresh(),source:'local',discovered_at:null}]).badge,'');
 const result=endpointFreshness({...fresh(),last_success_at:null,sampled_at:'invalid',discovered_at:null});
 assert.equal(result.pollAge,null);assert.equal(result.pollStale,true);assert.equal(result.discoveryStale,true);
});
test('collector failure overrides recently successful endpoint data',()=>{
 state.failure='offline';assert.equal(endpointFreshness(fresh()).unavailable,true);state.failure='';
});
