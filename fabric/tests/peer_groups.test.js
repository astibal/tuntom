"use strict";
const {readFileSync}=require("node:fs");
const {join}=require("node:path");
const {runInNewContext}=require("node:vm");
const {test}=require("node:test");
const assert=require("node:assert/strict");
const source=readFileSync(join(__dirname,"../static/app.js"),"utf8");
const group=runInNewContext(source.slice(source.indexOf("function peerNodes("),source.indexOf("function peerSecuritySummary("))+"\ngroupPeerRows");
const a={id:"a",name:"231s",kind:"tunnel",role:"server"}, b={...a,id:"b",name:"231_1s"}, c={...a,id:"c",name:"232s"}, sw={id:"sw",kind:"switch"};
const nodes=[{ip:"10.1.1.1",sources:["peer_access"],processes:["a","b"]}];
test("groups interleaved paths and promotes first visible path after filtering",()=>{
  const result=group([a,sw,b,c],nodes);
  assert.deepEqual(Array.from(result,x=>x.e.id),["a","b","sw","c"]);
  assert.equal(result[0].first,true); assert.equal(result[0].count,2);
  assert.equal(result[1].first,false); assert.equal(result[1].last,true);
  assert.equal(group([b,c],nodes)[0].first,true);
});
test("groups by numeric tunnel IDs without INFO, separates side and namespace",()=>{
  assert.equal(group([b,a],[])[0].e.id,"a");
  assert.equal(group([a,b],[])[0].count,2);
  for(const other of [{...b,name:"231_1c"},{...b,net_namespace:"other"},{...b,host:"other"},{...b,name:"unknown"}])
    assert.ok(group([a,other],nodes).every(x=>x.count===1));
  const result=group([{...b,name:"231_10s"},{...a,name:"231_2s"}],[]);
  assert.equal(result[0].e.name,"231_2s");
});
test("first member inherits metrics associated only with another path",()=>{
  const result=group([a,b],[{...nodes[0],processes:["b"]}]);
  assert.equal(result[0].peers.length,1);
});
