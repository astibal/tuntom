"use strict";
const {readFileSync}=require("node:fs");
const {runInNewContext}=require("node:vm");
const {test}=require("node:test");
const assert=require("node:assert/strict");
const {join}=require("node:path");

const source=readFileSync(join(__dirname,"../static/app.js"),"utf8");
const helpers=runInNewContext(source.slice(source.indexOf("function externalCert("),source.indexOf("function renderExternals("))+";({externalExpiry,externalHttpTone,externalLatencyTone})");

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
