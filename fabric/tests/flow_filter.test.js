const {test}=require('node:test');
const assert=require('node:assert/strict');
const {parse,matches}=require('../static/flow-filter.js');
const row={src:'10.20.30.17',dst:'2001:db8::7',src_port:'32001',dst_port:'443',protocol:'6',labels:['0x0000000000000011'],client_chain:'443'};
const match=(query,regex=false,r=row)=>matches(r,parse(query,regex));
test('aliases restrict side, family and ports without searching unrelated fields',()=>{
 for(const q of ['port 443','port:443','sport 32001','dport: 443','addr 2001:','addr6 2001:','src 10.20','saddr 10.20','dst 2001:','daddr 2001:','ip 10.20','ip6 2001:'])assert.ok(match(q),q);
 for(const q of ['sport 443','dport 32001','ip 2001:','ip6 10.20','src 2001:','dst 10.20','addr 32001','port 17','src6 10.20'])assert.ok(!match(q),q);
 assert.ok(match('addr "10.20.30.17"'));
});
test('network qualifiers use CIDR and exact host defaults',()=>{
 for(const q of ['net 10.20.30.0/24','snet 10.20.30.17','snet4 10.20.30.0/24','net6 2001:db8::/64','dnet6 2001:db8::7'])assert.ok(match(q),q);
 for(const q of ['dnet 10.20.30.0/24','snet 10.20.30.18','snet6 2001:db8::/64','dnet6 2001:db9::/64'])assert.ok(!match(q),q);
 for(const q of ['snet4 2001:db8::/64','net6 2001:db8::/129','net 1.2.3.4/33'])assert.throws(()=>parse(q));
 assert.ok(match('snet4 10.20.30.0/24',true));
});
test('regex uses individual values, respects scope, preserves case escapes and numeric labels',()=>{
 assert.ok(match('sport ^32\\d{3}$',true));
 assert.ok(match('dport ^(53|443)$',true));
 assert.ok(!match('sport ^(53|443)$',true));
 assert.ok(match('src ^10\\.20\\.',true));
 assert.ok(match('^17$',true));
 assert.ok(match('^0x11$',true));
 assert.ok(!match('^10.*443$',true));
 assert.ok(!match('src \\D+',true,{...row,src:'1234'}));
 assert.throws(()=>parse('sport [',true));
 assert.throws(()=>parse('sport '));
 assert.throws(()=>parse('x'.repeat(513),true));
});

test('incomplete network addresses fall back to scoped text matching',()=>{
 assert.ok(match('snet 10.20.'));
 assert.ok(match('net 10.20'));
 assert.ok(match('dnet6 2001:db8'));
 assert.ok(!match('snet6 2001:db8'));
 assert.ok(match('snet 10.20.',true));
 assert.throws(()=>parse('snet 10.20/24'));
});

test('port ranges include both boundaries and respect source/destination scopes',()=>{
 for(const q of ['port 32000-32999','sport: 32001-32001','dport 443-445','port 0-65535'])assert.ok(match(q),q);
 for(const q of ['sport 443-445','dport 32000-32999','port 444-32000','port 500-400','port 0-65536'])assert.ok(!match(q),q);
 assert.ok(match('sport 32000-32999',false,{...row,src_port:32000}));
 assert.ok(match('sport 32000-32999',false,{...row,src_port:32999}));
 assert.ok(!match('sport 32000-32999',false,{...row,src_port:33000}));
});
