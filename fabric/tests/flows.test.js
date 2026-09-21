const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const {runInNewContext}=require('node:vm');
const {test}=require('node:test');
const assert=require('node:assert/strict');
const source=readFileSync(join(__dirname,'../static/app.js'),'utf8');
const ipSource=source.slice(source.indexOf('function flowIPBucket('),source.indexOf('function flowCascadeBucket('));
const {flowLabel,flowMatches}=runInNewContext(ipSource+source.slice(source.indexOf('function flowLabel('),source.indexOf('function flowStack('))+';({flowLabel,flowMatches})',{FlowFilter:require('../static/flow-filter.js')});
test('flow labels preserve uint64 precision, stack order and exact decimal/hex search',()=>{
  const row={src:'::1',dst:'::2',protocol:'6',dst_port:'443',labels:['0xffffffffffffffff','0x0000000000000011']};
  assert.equal(flowLabel(row.labels[0],'dec'),'18446744073709551615');
  assert.equal(flowLabel(row.labels[0],'hex'),'0xffffffffffffffff');
  assert.ok(flowMatches(row,'::2 443 18446744073709551615'));
  assert.ok(flowMatches(row,'0XFFFFFFFFFFFFFFFF'));
  assert.ok(!flowMatches(row,'18446744073709551616'));
  assert.deepEqual(row.labels,['0xffffffffffffffff','0x0000000000000011']);
  assert.ok(flowMatches({labels:null},'unknown'));
});

test('VIA magic and cookie-bearing headers display symbolically in DEC and HEX',()=>{
 for(const label of ['0x5649410000','370596184064','0x4162435649410104',BigInt('0x4162435649410104').toString()]) {
  assert.equal(flowLabel(label,'dec'),'VIA');assert.equal(flowLabel(label,'hex'),'VIA');
  assert.ok(flowMatches({labels:[label]},'via'));
  assert.ok(flowMatches({labels:[label]},BigInt(label).toString()));
 }
 for(const label of ['0x4162435649410204','0x0062435649410104','0x4162435649410103'])assert.equal(flowLabel(label,'hex'),label);
});

test('only the saved portion of a complete VIA envelope is highlighted',()=>{
 const positions=runInNewContext(source.slice(source.indexOf('function flowLabel('),source.indexOf('function flowStack('))+';viaSavedPositions');
 const labels=['17','0x4162435649410105','0x100000200','123','99','42'];
 assert.deepEqual(Array.from(positions(labels)),[4,5]);
 assert.deepEqual(Array.from(positions(labels,true)),[1,2,3]);
 assert.deepEqual(Array.from(positions(labels.slice(0,5),true)),[]);
 assert.deepEqual(Array.from(positions(labels.slice(0,5))),[]);
 assert.deepEqual(Array.from(positions(['17','99','42'])),[]);
});

test('filters accept CIDR and port ranges shown by cascade summaries',()=>{
 const row={src:'10.20.30.17',dst:'192.0.2.8',src_port:'32456',dst_port:'53',protocol:'17',labels:['0x11']};
 assert.ok(flowMatches(row,'10.20.30.0/24 32000–32999 UDP 17'));
 assert.ok(flowMatches(row,'192.0.2.0/24 32000-32999'));
 assert.ok(!flowMatches(row,'10.20.31.0/24'));
 assert.ok(!flowMatches(row,'33000-33999'));
 assert.ok(!flowMatches(row,'TCP'));
 assert.throws(()=>flowMatches(row,'10.20.30.0/33'));
 assert.ok(flowMatches({...row,src:'2001:db8::abcd'},'2001:db8::/64'));
 assert.ok(!flowMatches({...row,src:'2001:db9::abcd'},'2001:db8::/64'));
});
