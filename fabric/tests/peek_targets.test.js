const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const {runInNewContext}=require('node:vm');
const {test}=require('node:test');
const assert=require('node:assert/strict');
const source=readFileSync(join(__dirname,'../static/app.js'),'utf8');
const parse=runInNewContext(source.slice(source.indexOf('function serviceTargets('),source.indexOf('function renderServices('))+';serviceTargets',{t:key=>key});
test('target editor preserves regex spaces, quotes and backslashes',()=>{
 const pattern='(?im)^service \\d+ "ready"$';
 const rows=parse('https://example.org/health 60 '+JSON.stringify(pattern)+'\ntcp://example.org:443 30');
 assert.equal(rows[0].payload_regex,pattern);assert.equal(rows[0].interval,60);
 assert.equal(rows[1].url,'tcp://example.org:443');assert.equal(rows[1].payload_regex,undefined);
 assert.equal(parse('tls://example.org')[0].interval,60);
 assert.throws(()=>parse('https://example.org 60 unquoted expression'));
});
const helpers=runInNewContext(source.slice(source.indexOf('function externalProtocol('),source.indexOf('function externalReply('))+';({externalGood})');
test('TCP and HTTP health does not depend on a TLS certificate',()=>{
 for(const protocol of ['tcp','http','dns'])assert.equal(helpers.externalGood({protocol,ok:true,available:true}),true);
 assert.equal(helpers.externalGood({protocol:'https',ok:true,available:true,tls:{trusted:false}}),false);
 assert.equal(helpers.externalGood({protocol:'http',ok:false,available:true,payload:{matched:false}}),false);
});
