const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const {runInNewContext}=require('node:vm');
const {test}=require('node:test');
const assert=require('node:assert/strict');
const source=readFileSync(join(__dirname,'../static/app.js'),'utf8');
const parse=runInNewContext(source.slice(source.indexOf('function topologyRules('),source.indexOf('const mapRules='))+';topologyRules');
test('canonical ordered rules retain drops, rewrites, masks, via and remote destinations',()=>{
 const rules='format 3\nswitch edge*, [17, &16, ...] to blocked drop\nswitch edge*, [17, &16, ...] to remote*, [99, *, ...] via [proxy] allow [id=test]\nswitch remote*, [99, ...] to edge*, [17, ...] allow';
 const rows=parse(rules,'edge_1');
 assert.equal(rows.length,3);
 assert.equal(rows[0].action,'drop');
 assert.equal(rows[1].stack,'[17, &16, ...]');
 assert.equal(rows[1].rewrite,'[99, *, ...]');
 assert.equal(rows[1].target,'remote*');
 assert.equal(rows[1].via,'[proxy]');
 assert.equal(rows[2].direction,'←');
 assert.equal(rows[2].stack,'[17, ...]');
 assert.equal(parse(rules,'unrelated').length,0);
});
test('unrestricted selectors, legacy labels, keyword ports and uint64 stay exact',()=>{
 assert.equal(parse('switch allow','a').length,2);
 assert.equal(parse('switch to remote allow','a')[0].target,'remote');
 assert.equal(parse('switch to, [18446744073709551615] to remote allow','to')[0].stack,'[18446744073709551615]');
 assert.equal(parse('switch edge,17 to remote,99 allow','edge')[0].stack,'17');
 assert.equal(parse('switch [42, <1, 8>, ...] to remote allow','edge')[0].stack,'[42, <1, 8>, ...]');
});
