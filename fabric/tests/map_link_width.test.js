const {readFileSync}=require("node:fs");
const {join}=require("node:path");
const {runInNewContext}=require("node:vm");
const {test}=require("node:test");
const assert=require("node:assert/strict");
const source=readFileSync(join(__dirname,"../static/app.js"),"utf8");
const width=runInNewContext(source.slice(source.indexOf("function mapLinkWidth("),source.indexOf("function mapConnector("))+";mapLinkWidth");
test("traffic width has a readable floor, grows monotonically and caps at 5 Gbps",()=>{
  for(const rate of [null,undefined,NaN,Infinity,-1,0]) assert.equal(width(rate),2);
  const rates=[0,1e6,1e8,1e9,5e9];
  for(let i=1;i<rates.length;i++) assert.ok(width(rates[i])>width(rates[i-1]));
  assert.equal(width(5e9),8);
  assert.equal(width(100e9),8);
});
