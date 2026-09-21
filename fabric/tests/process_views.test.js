const {test}=require('node:test');
const assert=require('node:assert/strict');
const {readFileSync}=require('node:fs');
const {runInNewContext}=require('node:vm');
const source=readFileSync(require('node:path').join(__dirname,'../static/app.js'),'utf8');
const {supportsProcessView,processViewSelection}=runInNewContext(source.slice(source.indexOf('function supportsProcessView('),source.indexOf('function renderProcesses('))+';({supportsProcessView,processViewSelection})');
const endpoints=['tunnel','switch','adapter','divert'].map(kind=>({id:kind,kind}));
test('specialized views include capable kinds even without samples or control socket',()=>{
 assert.deepEqual(endpoints.filter(e=>supportsProcessView(e,'flows')).map(e=>e.id),['adapter','divert']);
 assert.deepEqual(endpoints.filter(e=>supportsProcessView(e,'rules')).map(e=>e.id),['switch']);
 assert.equal(endpoints.filter(e=>supportsProcessView(e,'overview')).length,4);
});
test('selection follows view capabilities and handles process disappearance',()=>{
 assert.equal(processViewSelection(endpoints,'flows','tunnel'),'adapter');
 assert.equal(processViewSelection(endpoints,'flows','divert'),'divert');
 assert.equal(processViewSelection(endpoints,'rules','adapter'),'switch');
 assert.equal(processViewSelection(endpoints.filter(e=>e.kind==='tunnel'),'flows','tunnel'),null);
 assert.equal(processViewSelection([],'rules','switch'),null);
});
