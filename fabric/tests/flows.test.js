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
