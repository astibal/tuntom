const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const {runInNewContext}=require('node:vm');
const {test}=require('node:test');
const assert=require('node:assert/strict');
const source=readFileSync(join(__dirname,'../static/app.js'),'utf8');
const {viaServices,viaRate,viaShares}=runInNewContext(source.slice(source.indexOf('function viaServices('),source.indexOf('function renderViaBars('))+';({viaServices,viaRate,viaShares})',{outdated:e=>e.stale});
test('VIA roles come from explicit service relay selectors',()=>{
 const rows=viaServices('service proxy {\n client-relay in*\n server-relay out*\n}\nservice shared {\n relay common*\n}\nservice local {\n client-side c*\n server-side s*\n}');
 assert.equal(rows.length,2);
 assert.equal(rows[0].sides[0].name,'IN');assert.equal(rows[0].sides[1].pattern,'out*');
 assert.equal(rows[1].sides.length,1);assert.equal(rows[1].sides[0].name,'IN / OUT');
});
test('rates distinguish zero, missing, invalid and stale telemetry',()=>{
 const e={status:'reachable',metrics:{udp_rx_bps_5s:'10',udp_tx_bps_5s:'20',udp_rx_pps_5s:'0',udp_tx_pps_5s:'0'}};
 assert.equal(viaRate(e,'bps'),30);assert.equal(viaRate(e,'pps'),0);
 assert.equal(viaRate({...e,stale:true},'bps'),null);
 assert.equal(viaRate({...e,metrics:{udp_rx_bps_5s:'10'}},'bps'),null);
 assert.equal(viaRate({...e,metrics:{udp_rx_bps_5s:'NaN',udp_tx_bps_5s:'0'}},'bps'),null);
});
test('shares do not pretend incomplete observations are complete',()=>{
 assert.deepEqual(Array.from(viaShares([90,10])),[90,10]);
 assert.deepEqual(Array.from(viaShares([0,0])),[0,0]);
 assert.deepEqual(Array.from(viaShares([90,null])),[null,null]);
});
