const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const {runInNewContext}=require('node:vm');
const {test}=require('node:test');
const assert=require('node:assert/strict');
const source=readFileSync(join(__dirname,'../static/app.js'),'utf8');
const html=readFileSync(join(__dirname,'../static/index.html'),'utf8');
const catalog=runInNewContext(source.slice(0,source.indexOf('function diagnostic('))+';({messages,t,setLanguage:value=>language=value})');
test('every translation has Czech, English and French with matching parameters',()=>{
 const parameters=text=>[...text.matchAll(/\{(\w+)\}/g)].map(m=>m[1]).sort();
 for(const [key,values] of Object.entries(catalog.messages)){
  assert.equal(values.length,3,key);
  for(const value of values){assert.ok(typeof value==='string'&&value.trim(),key);assert.deepEqual(parameters(value),parameters(values[0]),key);}
 }
});
test('all static translation attributes resolve to catalog entries',()=>{
 for(const match of html.matchAll(/data-i18n(?:-placeholder|-aria|-title)?="([^"]+)"/g))assert.ok(catalog.messages[match[1]],match[1]);
});
test('administration and deployment actions switch between English and French',()=>{
 catalog.setLanguage('en');assert.equal(catalog.t('uiCreateTunnel'),'Create tunnel');
 catalog.setLanguage('fr');assert.equal(catalog.t('uiCreateTunnel'),'Créer un tunnel');
});
