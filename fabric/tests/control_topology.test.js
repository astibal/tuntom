const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const {runInNewContext}=require('node:vm');
const {test}=require('node:test');
const assert=require('node:assert/strict');
const source=readFileSync(join(__dirname,'../static/app.js'),'utf8');
const {controlTopology,observedPlacement}=runInNewContext(source.slice(source.indexOf('function controlTopology('),source.indexOf('function observedGroups('))+';({controlTopology,observedPlacement})');
const local={id:'sw',kind:'switch'};
const remote=(id,path)=>({id,source:'discovered',control_routes:[{origin:'sw',path}]});
const attachment={source:'tun',target:'sw',port_id:'a/b',namespace_verified:true};
test('discovery connects adjacent hops through verified local attachment, with no duplicate cards or edges',()=>{
 const nodes=[local,{id:'tun'},remote('peer','port:a%2Fb/peer'),remote('remote-sw','port:a%2Fb/peer/port:switch')];
 nodes[2].control_routes.push({origin:'sw',path:'port:other'});
 const links=controlTopology(nodes,[attachment]);
 assert.ok(links.some(l=>l.source==='tun' && l.target==='peer' && l.kind==='control'));
 assert.ok(links.some(l=>l.source==='peer' && l.target==='remote-sw'));
 assert.ok(links.some(l=>l.source==='sw' && l.target==='peer'));
 assert.equal(links.length,4);
});
test('missing or ambiguous prefix never invents a direct link',()=>{
 assert.equal(controlTopology([local,remote('deep','port:missing/peer')],[]).length,0);
 assert.equal(controlTopology([local,remote('a','port:x'),remote('b','port:x'),remote('c','port:x/peer')],[]).filter(l=>l.target==='c').length,0);
 assert.equal(controlTopology([local,{id:'tun'},remote('peer','port:a%2Fb/peer')],[{...attachment,namespace_verified:false}]).length,1);
});
test('forest handles cycles and disconnected nodes, stable across link order, with no overlaps',()=>{
 const groups=['sw','a','b','c','isolated'].map(id=>({key:id,members:[{id,kind:id==='sw'?'switch':'tunnel'}]}));
 const links=[['sw','a'],['sw','b'],['a','c'],['b','c']].map(([source,target])=>({source,target}));
 const heights=new Map(groups.map(g=>[g.key,g.key==='a'?180:66]));
 const result=observedPlacement(groups,links,heights);
 assert.equal(result.placed.size,5);
 assert.equal(result.placed.get('sw').col,0);
 assert.equal(result.placed.get('c').col,2);
 assert.deepEqual([...result.placed], [...observedPlacement(groups,[...links].reverse(),heights).placed]);
 for(const [a,p] of result.placed)for(const [b,q] of result.placed)if(a!==b && p.col===q.col)
   assert.ok(p.y+heights.get(a)<=q.y || q.y+heights.get(b)<=p.y);
});
