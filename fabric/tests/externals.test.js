"use strict";
const {readFileSync}=require("node:fs");
const {runInNewContext}=require("node:vm");
const {test}=require("node:test");
const assert=require("node:assert/strict");
const {join}=require("node:path");

const source=readFileSync(join(__dirname,"../static/app.js"),"utf8");
const helpers=runInNewContext(source.slice(source.indexOf("function externalCert("),source.indexOf("function renderExternals("))+";({externalExpiry,externalHttpTone,externalLatencyTone})");
const chartHelpers=runInNewContext(source.slice(source.indexOf("function robustChartY("),source.indexOf("function chartValue("))+";({robustChartY})");

test("external warning thresholds distinguish certificate, HTTP and latency severity",()=>{
  const expiry=days=>helpers.externalExpiry({tls:{certificate:{not_after:"2099-01-01T00:00:00Z",days_remaining:days}}})[1];
  assert.equal(expiry(15),"ok");
  assert.equal(expiry(14),"warn");
  assert.equal(expiry(8),"warn");
  assert.equal(expiry(7),"bad");
  assert.equal(expiry(-1),"bad");
  assert.equal(helpers.externalHttpTone(200),"ok");
  assert.equal(helpers.externalHttpTone(404),"warn");
  assert.equal(helpers.externalHttpTone(503),"bad");
  assert.equal(helpers.externalHttpTone(undefined),"bad");
  assert.equal(helpers.externalLatencyTone(300,300,1000),"ok");
  assert.equal(helpers.externalLatencyTone(301,300,1000),"warn");
  assert.equal(helpers.externalLatencyTone(1001,300,1000),"bad");
});

test("external chart robust scale does not let one outlier flatten the graph",()=>{
  const samples=[10,11,12,10,11,12,10,11,12,10000].map(value=>({min:value,max:value}));
  const [low,high]=chartHelpers.robustChartY(samples.flatMap(sample=>[sample.min,sample.max]),0);
  assert.ok(low>=0);
  assert.ok(high<100);
});

const dnsHelpers=runInNewContext(source.slice(source.indexOf('function externalProtocol('),source.indexOf('function externalCert('))+';({externalGood,externalReply,externalTrust})');
test('DNS service result does not require TLS for plain DNS and respects DNS assertions',()=>{
  assert.equal(dnsHelpers.externalGood({url:'dns://resolver/?name=x',ok:true,available:true}),true);
  assert.equal(dnsHelpers.externalGood({url:'dot://resolver/?name=x',ok:true,available:true}),false);
  assert.equal(dnsHelpers.externalGood({protocol:'doh',ok:false,available:true,tls:{trusted:true}}),false);
  assert.equal(dnsHelpers.externalTrust({protocol:'dns'}),'N/A');
  assert.equal(dnsHelpers.externalReply({dns:{rcode:'NOERROR',type:'AAAA',ad:true}}),'NOERROR · AAAA · AD ✓');
});
const dnsBuilder=runInNewContext(source.slice(source.indexOf('function dnsTargetURL('),source.indexOf("$('service-dns-protocol').addEventListener"))+';dnsTargetURL',{URLSearchParams});
test('DNS builder encodes explicit resolver port, path and query assertions',()=>{
  const value=dnsBuilder('doh','resolver.example',8443,'/dns-query','some.domain.tld','AAAA',true,'2001:db8::1');
  const url=new URL(value);assert.equal(url.port,'8443');assert.equal(url.pathname,'/dns-query');
  assert.equal(url.searchParams.get('expect'),'2001:db8::1');assert.equal(url.searchParams.get('ad'),'1');
  assert.throws(()=>dnsBuilder('dns','resolver.example',0,'/','x','A',false,''));
});
