"use strict";
const {readFileSync} = require("node:fs");
const {runInNewContext} = require("node:vm");
const {test} = require("node:test");
const assert = require("node:assert/strict");
const {join} = require("node:path");

const source = readFileSync(join(__dirname,"../static/app.js"),"utf8");
const {ThroughputHistory,chartSeries,nearestChartSample,chartIssueBuckets} = runInNewContext(source.slice(
  source.indexOf("class ThroughputHistory"),source.indexOf("class WarningHistory")) +
  "\n({ThroughputHistory,chartSeries,nearestChartSample,chartIssueBuckets})");
const day = 86400000;
const point = (time,rx=10,tx=20) => ({time,rx,tx});

test("retains 24 hours; selecting shorter windows preserves history and duplicate samples do not grow it",()=>{
  const history = new ThroughputHistory();
  for (let time=0;time<=day+5000;time+=5000) history.add(point(time),time);
  assert.equal(history.samples.length,17281);
  assert.equal(history.samples[0].time,5000);
  assert.equal(history.window(300000,day+5000).length,61);
  assert.equal(history.window(3600000,day+5000).length,721);
  assert.equal(history.window(day,day+5000).length,17281);
  history.add(point(day+5000,99),day+5000);
  history.add(point(day),day+5000);
  history.add(point(NaN),day+5000);
  assert.equal(history.samples.length,17281);
  assert.equal(history.samples.at(-1).rx,10);
  assert.equal(history.window(day,day*2+5001).length,0);
});

test("long-range rendering stays bounded and preserves narrow RX/TX spikes and troughs",()=>{
  const points = Array.from({length:86401},(_,i)=>point(i*1000));
  points[12345].rx=9000; points[12346].rx=0;
  points[67890].tx=8000; points[67891].tx=0;
  for (const [key,peak] of [["rx",9000],["tx",8000]]) {
    const series=chartSeries(points,key,0,day,15000);
    assert.ok(series.length<=4*577);
    assert.equal(Math.max(...series.map(p=>p[key])),peak);
    assert.equal(Math.min(...series.map(p=>p[key])),0);
    assert.equal(series[0].time,0);
    assert.equal(series.at(-1).time,day);
    assert.ok(series.every((p,i)=>!i || p.time>series[i-1].time));
  }
});

test("gaps survive downsampling, while zero is data and missing RX does not break TX",()=>{
  const points=[point(0,0),point(5000,null),point(10000,15),point(40000,30),point(45000,0)];
  const rx=chartSeries(points,"rx",0,day,15000);
  const tx=chartSeries(points,"tx",0,day,15000);
  assert.equal(rx.filter(p=>p===null).length,2);
  assert.equal(tx.filter(p=>p===null).length,1);
  assert.equal(rx[0].rx,0);
  assert.equal(rx.at(-1).rx,0);
  assert.equal(chartSeries([],"rx",0,day,15000).length,0);
  assert.equal(chartSeries([point(0,null,null)],"rx",0,day,15000).length,0);
});

test("inspection reads the original sample and refuses to interpolate across gaps",()=>{
  const points=[point(1000,0),point(6000,987654321.125),point(90000,null)];
  assert.equal(nearestChartSample(points,5900,7500),points[1]);
  assert.equal(nearestChartSample(points,1000,7500).rx,0);
  assert.equal(nearestChartSample(points,30000,7500),null);
  assert.equal(nearestChartSample(points,-9000,7500),null);
  assert.equal(nearestChartSample(points,90000,7500).rx,null);
  assert.equal(nearestChartSample(points,6000,0),points[1]);
  assert.equal(nearestChartSample(points.slice(1),1000,0),null); // Expired pin must not jump to its neighbour.
  assert.equal(nearestChartSample([],1000),null);
});

test("issue markers retain every original cause, including missing metrics, with bounded drawing",()=>{
  const points=Array.from({length:17281},(_,i)=>({...point(i*5000),
    issues:[{key:"errors",counters:{pool_stalls:String(i)}}],interval:5,cpu_0:i%100}));
  points[10].rx=null; points[10].tx=null;
  const history=new ThroughputHistory();
  for (const p of points) history.add(p,p.time);
  const groups=chartIssueBuckets(history.window(day,day),0,day,1000);
  assert.ok(groups.length<=85);
  assert.equal(groups.flat().length,points.length);
  assert.equal(groups.flat()[10],points[10]);
  assert.equal(groups.flat()[10].issues[0].counters.pool_stalls,"10");
  // Markers live with chart history, not the separately dismissible 5-minute balloons.
  assert.equal(chartIssueBuckets(history.window(300000,day),day-300000,day,1000).flat().length,61);
  assert.equal(chartIssueBuckets(history.window(day,day*2+1),day+1,day*2+1,1000).length,0);
});

test("worker histories preserve missing CPU as gaps and separate process histories",()=>{
  const a=new ThroughputHistory(),b=new ThroughputHistory();
  a.add({...point(0),cpu_0:0,issues:[{code:"session_down"}]},0);
  a.add({...point(5000),cpu_0:null},5000);
  a.add({...point(10000),cpu_0:75},10000);
  b.add({...point(0),cpu_0:25},0);
  const series=chartSeries(a.samples,"cpu_0",0,10000,15000);
  assert.equal(series[0].cpu_0,0);
  assert.equal(series[1],null);
  assert.equal(series.at(-1).cpu_0,75);
  assert.equal(chartIssueBuckets(b.samples,0,10000,1000).length,0);
});

test("chart renders before discovery has selected any endpoint",()=>{
  const svg={clientWidth:600,clientHeight:220,innerHTML:"",setAttribute(){}};
  const context={state:{history:new Map(),chartRange:300000,chartNow:300000},
    chartSeries,chartIssueBuckets,renderChartReadout(){},esc:String,bps:String,locale:()=>"en-GB",t:key=>key};
  const draw=runInNewContext(source.slice(source.indexOf("const chartViews ="),
    source.indexOf("function renderChartReadout"))+"; drawTimeChart",context);
  assert.doesNotThrow(()=>draw(svg,undefined));
  assert.doesNotThrow(()=>draw(svg,"new-process"));
  assert.ok(svg.innerHTML.includes('class="grid"'));
  assert.ok(!/NaN|Infinity/.test(svg.innerHTML));
});

test("background polling continues and returning to the page refreshes immediately",()=>{
  const listeners={},timers=[];
  let refreshes=0,draws=0;
  const document={hidden:true,addEventListener:(name,fn)=>listeners[name]=fn};
  runInNewContext(source.slice(source.indexOf("function resumeVisiblePage()")),{
    document,window:{addEventListener:(name,fn)=>listeners[name]=fn},
    setInterval:fn=>timers.push(fn),refresh:()=>refreshes++,drawChart:()=>draws++,renderWarnings(){}
  });
  timers[0]();
  assert.equal(refreshes,1); // Hidden pages must not deliberately discard observations.
  listeners.visibilitychange();
  assert.equal(draws,0);
  document.hidden=false;
  listeners.visibilitychange();
  assert.equal(refreshes,2);
  assert.equal(draws,1);
  listeners.pageshow();
  assert.equal(refreshes,3);
});

test("returning to overview redraws after revealing the chart, even while paused",()=>{
  const views=Object.fromEntries(["overview","metrics","rules","switch","diagnostics","syspiper"].map(name=>
    ["view-"+name,{hidden:name!=="metrics",scrollIntoView(){}}]));
  let draws=0;
  const show=runInNewContext(source.slice(source.indexOf("function showView("),source.indexOf("function selectProcess("))+";showView",{
    state:{view:"metrics",paused:true},document:{querySelectorAll:()=>[]},$:id=>views[id],
    drawChart(){assert.equal(views["view-overview"].hidden,false);draws++;}
  });
  show("overview");
  assert.equal(draws,1);
  assert.equal(views["view-metrics"].hidden,true);
});

test("persisted history merges with live points without duplicates or losing worker incidents",()=>{
  const history=new ThroughputHistory();
  history.add(point(10000,1),10000);
  history.merge([point(5000,0),{...point(10000,2),cpu_0:50,issues:[{code:'telemetry_missing'}]}],10000);
  history.merge([point(5000,0)],10000);
  assert.equal(history.samples.length,2);
  assert.equal(history.samples[0].time,5000);
  assert.equal(history.samples[1].rx,2);
  assert.equal(history.samples[1].cpu_0,50);
  assert.equal(history.samples[1].issues[0].code,'telemetry_missing');
  history.merge([point(10000+day+1)],10000+day+1);
  assert.equal(history.samples.length,1);
});

test("history loader fills all pages, keeps live samples and retries without API disconnect",async()=>{
  const now=Date.now(),history=new ThroughputHistory(),calls=[];
  history.add(point(now),now);
  const state={paused:false,data:{history:{enabled:true}},historyLoads:new Map(),history:new Map([['id',history]]),chartNow:now};
  let fail=false,draws=0;
  const load=runInNewContext(source.slice(source.indexOf('async function loadHistory('),source.indexOf('const chartViews ='))+';loadHistory',{
    state,ThroughputHistory,drawChart:()=>draws++,api:async url=>{
      calls.push(url);
      if(fail) throw Error('offline');
      return calls.length===1 ? {samples:[point(now-10000)],until:now,next_after:now-10000} :
        {samples:[point(now-5000)],until:now,next_after:null};
    }
  });
  await load('id');
  assert.equal(calls.length,2);
  assert.ok(calls[1].includes('until='+now));
  assert.equal(history.samples.length,3);
  assert.equal(history.samples.at(-1).time,now);
  fail=true;state.historyLoads.get('id').retry=0;
  await load('id');
  assert.equal(state.historyLoads.get('id').error,true);
  assert.equal(state.failure,undefined);
  assert.equal(history.samples.length,3);
  assert.equal(draws,2);
});

test("system percentage charts use saved RAM samples and the independent poll interval",()=>{
  const id='syspiper:192.0.2.1:8181',history=new ThroughputHistory();
  history.add({time:1000,ram:8.5},1000);
  const svg={clientWidth:600,clientHeight:220,innerHTML:'',setAttribute(){}};
  const context={state:{history:new Map([[id,history]]),chartRange:300000,chartNow:2000,data:{poll_interval_seconds:5,syspiper:{interval_seconds:30}}},
    chartSeries,chartIssueBuckets,renderChartReadout(){},esc:String,bps:String,locale:()=>"en-GB",t:key=>key};
  const chart=runInNewContext(source.slice(source.indexOf("const chartViews ="),source.indexOf("function renderChartReadout"))+";({drawTimeChart,chartViews})",context);
  chart.drawTimeChart(svg,id,'ram',true);
  const model=chart.chartViews.get(svg);
  assert.equal(model.keys[0],'ram');
  assert.equal(model.gap,90000);
  assert.equal(model.max,100);
  assert.match(svg.innerHTML,/class="rx" d="M/);
  assert.ok(!/NaN|Infinity/.test(svg.innerHTML));
});
