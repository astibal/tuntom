const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const {runInNewContext}=require('node:vm');
const {test}=require('node:test');
const assert=require('node:assert/strict');
const source=readFileSync(join(__dirname,'../static/app.js'),'utf8');
const {flowIPBucket,flowCascadeBucket,buildFlowCascade,flowCascadeChoices,compactFlowGroup}=runInNewContext(source.slice(source.indexOf('function flowIPBucket('),source.indexOf('const flowCascadeOpen='))+';({flowIPBucket,flowCascadeBucket,buildFlowCascade,flowCascadeChoices,compactFlowGroup})');
const settings={src4:'24',dst4:'24',src6:'64',dst6:'64',SPORT:'1000',DPORT:'1000'};
const row=(src,port,extra={})=>({src,src_port:String(port),dst:'192.0.2.1',dst_port:'53',protocol:'17',labels:['17'],...extra});
test('IPv4 and IPv6 networks, aliases, mapped addresses and family separation',()=>{
 assert.equal(flowIPBucket('10.20.30.254','24','64').label,'10.20.30.0/24');
 assert.equal(flowIPBucket('255.255.255.255','0','64').label,'0.0.0.0/0');
 assert.equal(flowIPBucket('10.20.30.254','32','64').label,'10.20.30.254/32');
 assert.equal(flowIPBucket('2001:db8::abcd','24','64').key,flowIPBucket('2001:0db8:0:0:0:0:0:1','24','64').key);
 assert.equal(flowIPBucket('::','24','0').label,'::/0');
 assert.equal(flowIPBucket('::ffff:192.0.2.1','24','128').label,'::ffff:c000:201/128');
 assert.notEqual(flowIPBucket('0.0.0.0','0','0').key,flowIPBucket('::','0','0').key);
 assert.equal(flowIPBucket('192.0.2.1','none','64'),null);
 assert.equal(flowIPBucket('::1','24','none'),null);
 for(const bad of ['999.1.2.3','2001:::1','1:2:3','::1::2'])assert.ok(flowIPBucket(bad,'24','64').key.startsWith('invalid:'));
});
test('port bucket boundaries and missing ports',()=>{
 for(const [value,label] of [[0,'0–999'],[999,'0–999'],[1000,'1000–1999'],[65535,'65000–65535']])assert.equal(flowCascadeBucket(row('a',value),'SPORT',settings).label,label);
 assert.equal(flowCascadeBucket({src_port:'65535'},'SPORT',{...settings,SPORT:'65536'}).label,'0–65535');
 assert.equal(flowCascadeBucket({},'SPORT',settings).key,'missing');
});
test('counts are scoped to parents; original labels and context survive at leaves',()=>{
 const rows=[row('10.0.1.1',1001),row('10.0.1.2',1002,{labels:['99'],client_chain:'7'}),row('10.0.2.1',1001)];
 const tree=buildFlowCascade(rows,['SRC','SPORT'],settings);
 assert.equal(tree.children.size,2);assert.equal(tree.count,3);
 const a=[...tree.children.values()][0];assert.equal(a.count,2);assert.equal(a.ports.size,2);assert.equal(a.sources.size,2);
 const leaf=[...a.children.values()][0];assert.equal(leaf.rows[1],rows[1]);
 const reverse=buildFlowCascade(rows,['SPORT','SRC'],settings);assert.equal(reverse.children.size,1);assert.equal([...reverse.children.values()][0].children.size,2);
 const refreshed=buildFlowCascade([...rows].reverse(),['SRC','SPORT'],settings);
 assert.deepEqual([...tree.children.keys()].sort(),[...refreshed.children.keys()].sort());
});
test('none skips levels, raw rows retain order, duplicate choice only clears rightwards',()=>{
 const rows=[row('10.0.1.1',1001),row('10.0.1.2',3000)];
 const skipped=buildFlowCascade(rows,['SRC','','SPORT'],{...settings,src4:'none',src6:'none'});
 assert.equal([...skipped.children.values()][0].field,'SPORT');
 const flat=buildFlowCascade(rows,[],settings);assert.deepEqual(Array.from(flat.rows),rows);
 assert.deepEqual(Array.from(flowCascadeChoices(['SRC','SPORT','DST','DPORT'],0,'DST')),['DST','SPORT','','DPORT']);
});
test('large snapshot has exact counts and no dropped leaves',()=>{
 const rows=Array.from({length:20000},(_,i)=>row('10.'+(i%256)+'.0.1',i%65536));
 const tree=buildFlowCascade(rows,['SRC','SPORT','DST','DPORT'],settings);
 const visit=n=>n.rows.length+[...n.children.values()].reduce((sum,c)=>sum+visit(c),0);
 assert.equal(visit(tree),rows.length);assert.equal(tree.count,rows.length);
});

test('unary levels collapse but branching and exact row context remain',()=>{
 const rows=[row('10.0.1.1',32001),row('10.0.1.2',32002,{labels:['99'],client_chain:'7'})];
 const tree=buildFlowCascade(rows,['SRC','SPORT','DST','DPORT'],settings);
 const group=[...tree.children.values()][0],compact=compactFlowGroup(group);
 assert.equal(compact.children.length,2);assert.equal(compact.children[0],rows[0]);
 assert.equal(compact.ranges.SPORT,'32000–32999');
 assert.equal(group.summary.dst.size,1);assert.equal(group.summary.dst_port.size,1);
 assert.equal(group.summary.stacks.size,2);assert.equal(group.summary.context.size,2);
 const branched=buildFlowCascade([...rows,row('10.0.1.3',34001)],['SRC','SPORT','DST'],settings);
 const branches=compactFlowGroup([...branched.children.values()][0]);
 assert.equal(branches.children.length,2);assert.ok(branches.children.every(child=>child.children));
});
