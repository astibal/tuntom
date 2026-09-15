"use strict";
const {readFileSync} = require("node:fs");
const {runInNewContext} = require("node:vm");
const {test} = require("node:test");
const assert = require("node:assert/strict");
const {join} = require("node:path");

const source = readFileSync(join(__dirname,"../static/app.js"),"utf8");
const {ThroughputHistory,chartSeries} = runInNewContext(source.slice(
  source.indexOf("class ThroughputHistory"),source.indexOf("class WarningHistory")) +
  "\n({ThroughputHistory,chartSeries})");
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
