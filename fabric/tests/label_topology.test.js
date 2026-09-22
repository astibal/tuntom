const {test}=require('node:test');
const assert=require('node:assert/strict');
const {readFileSync}=require('node:fs');
const {runInNewContext}=require('node:vm');
const source=readFileSync(require('node:path').join(__dirname,'../static/app.js'),'utf8');
const parsers=runInNewContext(source.slice(source.indexOf('function switchTopologyRules('),source.indexOf('const mapRules='))+';({switchTopologyRules,topologyRules,classifierTopologyRules,labelStackMatches,rewrittenLabelStack,classifierRouteLinks})',{TextEncoder});

test('label topology keeps ordered switch directions and rewrites',()=>{
  const rows=parsers.switchTopologyRules('format 2\nswitch edge*, [17, ...] to inet*, [99, ...] allow\nswitch edge* to blocked* drop');
  assert.deepEqual(JSON.parse(JSON.stringify(rows.map(row=>[row.source,row.stack,row.target,row.rewrite,row.action]))),[
    ['edge*','[17, ...]','inet*','[99, ...]','allow'],
    ['edge*','*','blocked*','*','drop']
  ]);
  assert.equal(parsers.topologyRules('switch a, [1] to b, [2] allow','b')[0].direction,'←');
});

test('label topology parses classifier matches, unconditional rules and quoted comments',()=>{
  const rows=parsers.classifierTopologyRules('format 1\nclassify ip4 proto tcp dport 443 to [17, "A#B"] # note\nclassify to [99]');
  assert.deepEqual(JSON.parse(JSON.stringify(rows.map(row=>[row.match,row.stack]))),[
    ['ip4 proto tcp dport 443','[17, "A#B"]'],
    ['*','[99]']
  ]);
});

test('classifier outputs highlight forward and rewritten return rules separately',()=>{
  const routes=parsers.switchTopologyRules('format 2\nswitch edge*, [17, ...] to inet*, [99, ...] allow\nswitch inet*, [99, ...] to edge*, [17, ...] allow');
  const links=parsers.classifierRouteLinks(['edge0'],'[17, 42]',routes,['edge0','inet0']);
  assert.deepEqual(Array.from(links.forward),[0]);
  assert.deepEqual(Array.from(links.returns),[1]);
  assert.equal(links.missingReturn,false);
  assert.equal(parsers.rewrittenLabelStack('[17, 42]',routes[0]),'[99, 42]');
});

test('return warning requires an allow, while matching return drops remain visible',()=>{
  const routes=parsers.switchTopologyRules('switch edge, [17] to inet, [99] allow\nswitch inet, [99] to edge, [17] drop');
  const links=parsers.classifierRouteLinks(['edge'],'[17]',routes,['edge','inet']);
  assert.deepEqual(Array.from(links.returns),[1]);
  assert.equal(links.missingReturn,true);
  assert.equal(parsers.classifierRouteLinks(['edge'],'[17]',parsers.switchTopologyRules('switch allow'),['edge','inet']).missingReturn,false);
});

test('visual matching follows exact, wildcard, rest, mask, range and literal syntax',()=>{
  assert.equal(parsers.labelStackMatches('[17, 24, 9]','[17, &16, ...]'),true);
  assert.equal(parsers.labelStackMatches('[0x11, b1100]','[17, <8,15>]'),true);
  assert.equal(parsers.labelStackMatches('[17, 7]','[17]'),false);
  assert.equal(parsers.labelStackMatches('["AB"]','[0x4142000000000000]'),true);
});
