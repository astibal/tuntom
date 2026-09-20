const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const {runInNewContext}=require('node:vm');
const {test}=require('node:test');
const assert=require('node:assert/strict');
const source=readFileSync(join(__dirname,'../static/app.js'),'utf8');
const {flowLabel,flowMatches}=runInNewContext(source.slice(source.indexOf('function flowLabel('),source.indexOf('function flowStack('))+';({flowLabel,flowMatches})');
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
