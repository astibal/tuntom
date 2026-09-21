/* Shared by the UI tests and a disposable worker; no regular expressions run on the UI thread. */
(function(root) {
'use strict';
const aliases={port:['src_port','dst_port'],sport:['src_port'],dport:['dst_port'],
 ip:['src','dst'],ip4:['src','dst'],ip6:['src','dst'],addr:['src','dst'],addr4:['src','dst'],addr6:['src','dst'],
 src:['src'],dst:['dst'],saddr:['src'],daddr:['dst'],src4:['src'],dst4:['dst'],src6:['src'],dst6:['dst'],saddr4:['src'],daddr4:['dst'],saddr6:['src'],daddr6:['dst'],
 net:['src','dst'],snet:['src'],dnet:['dst'],net4:['src','dst'],snet4:['src'],dnet4:['dst'],net6:['src','dst'],snet6:['src'],dnet6:['dst']};
function parse(query,regex=false) {
 let body=query.trim(),category='';
 const match=body.match(/^([a-z][a-z0-9]*)(?::\s*|\s+)([\s\S]*)$/i);
 if(match && Object.hasOwn(aliases,match[1].toLowerCase())){category=match[1].toLowerCase();body=match[2].trim();}
 if(Object.hasOwn(aliases,body.toLowerCase()) && !category)throw Error('missing');
 if(category && !body)throw Error('missing');
 if(body.length>=2 && ((body[0]==='"' && body.at(-1)==='"') || (body[0]==="'" && body.at(-1)==="'")))body=body.slice(1,-1);
 const family=category==='ip' || category.endsWith('4')?4:category.endsWith('6')?6:0;
 if(body.length>512)throw Error('long');
 const networkCategory=category.includes('net');
 const partial=networkCategory && !body.includes('/') && flowIPBucket(body,'32','128')?.key.startsWith('invalid:');
 const network=networkCategory && !partial;
 const parts=network?[body]:partial?[body.toLowerCase()]:regex?[]:body.toLowerCase().split(/\s+/).filter(Boolean);
 for(const part of parts)if(network || part.includes('/')) {
  if(!network && !/^.+\/\d+$/.test(part))continue;
  networkSpec(part,family);
 }
 let expression=null;
 if(regex && !networkCategory && body) {try{expression=new RegExp(body,'i');}catch{throw Error('regex');}}
 return {category,fields:aliases[category],body,family,network,parts,expression};
}
function networkSpec(value,family=0) {
 const parts=value.split('/'),address=parts[0],v6=address.includes(':'),bits=v6?128:32;
 if(parts.length>2 || (family && family!==(v6?6:4)))throw Error('network');
 const prefix=parts.length===1?bits:/^\d+$/.test(parts[1])?Number(parts[1]):-1;
 if(prefix<0 || prefix>bits)throw Error('network');
 const bucket=flowIPBucket(address,String(prefix),String(prefix));
 if(!bucket || bucket.key.startsWith('invalid:') || bucket.key==='missing')throw Error('network');
 return {key:bucket.key,prefix,v6};
}
function inNetwork(address,spec) {
 return typeof address==='string' && address.includes(':')===spec.v6 && flowIPBucket(address,String(spec.prefix),String(spec.prefix))?.key===spec.key;
}
function labelValues(label) {
 const n=BigInt(label),cookie=[40n,48n,56n].map(s=>Number((n>>s)&255n));
 const via=n===0x5649410000n || ((n&0xffffff0000n)===0x5649410000n && ((n>>8n)&255n)===1n && (n&255n)>=4n && (n&255n)<=7n && n>=0n && n<=0xffffffffffffffffn && cookie.every(c=>c>=65&&c<=90 || c>=97&&c<=122));
 return [String(label),n.toString(),'0x'+n.toString(16),...(via?['VIA']:[])];
}
function matches(row,filter) {
 let values=filter.fields?filter.fields.map(key=>row[key]).filter(v=>v!==undefined && v!==null):
 Object.values(row).flatMap(v=>Array.isArray(v)?v.flatMap(labelValues):[v??'unknown']);
 if(filter.family)values=values.filter(v=>typeof v==='string' && v.includes(':')===(filter.family===6));
 if(!filter.fields)values.push(({6:'TCP',17:'UDP',1:'ICMP',58:'ICMPv6'}[row.protocol] || ''),row.protocol===undefined?'L3':'');
 values=values.map(String);
 if(filter.network){const spec=networkSpec(filter.body,filter.family);return values.some(v=>inNetwork(v,spec));}
 if(filter.expression)return values.some(v=>filter.expression.test(v));
 const text=values.join(' ').toLowerCase();
 return filter.parts.every(part=>{
  if(/^.+\/\d+$/.test(part)) {
   const spec=networkSpec(part,filter.family);
   return (filter.fields?values:[row.src,row.dst]).some(v=>inNetwork(v,spec));
  }
  const range=part.match(/^(\d{1,5})[-–](\d{1,5})$/);
  if(range && (!filter.category || filter.category.endsWith('port'))) {
   const lo=Number(range[1]),hi=Number(range[2]);
   return lo<=hi && hi<=65535 && (filter.fields?values:[row.src_port,row.dst_port]).some(p=>p!==undefined && p!==null && Number(p)>=lo && Number(p)<=hi);
  }
  return text.includes(part);
 });
}
function flowIPBucket(value, prefix4, prefix6) {
  if(prefix4==="none" && prefix6==="none")return null;
  if(typeof value!=="string")return {key:"missing",label:"—"};
  const v4=ip=>{const parts=ip.split('.');return parts.length===4 && parts.every(p=>/^\d{1,3}$/.test(p) && Number(p)<=255)?parts.reduce((n,p)=>(n<<8n)|BigInt(p),0n):null;};
  let bits=32,n=v4(value),prefix=prefix4;
  if(value.includes(':')) {
    bits=128;prefix=prefix6;let ip=value.toLowerCase();
    if(ip.includes('.')) {
      const last=ip.lastIndexOf(':'),tail=v4(ip.slice(last+1));
      if(tail===null)return {key:'invalid:'+value,label:value};
      ip=ip.slice(0,last+1)+(tail>>16n).toString(16)+':'+(tail&65535n).toString(16);
    }
    const halves=ip.split('::'),left=halves[0]?halves[0].split(':'):[],right=halves[1]?halves[1].split(':'):[];
    if(halves.length>2 || ![...left,...right].every(p=>/^[0-9a-f]{1,4}$/.test(p)) ||
      (halves.length===1?left.length!==8:left.length+right.length>=8))return {key:'invalid:'+value,label:value};
    const words=halves.length===1?left:[...left,...Array(8-left.length-right.length).fill('0'),...right];
    n=words.reduce((acc,p)=>(acc<<16n)|BigInt('0x'+p),0n);
  }
  if(n===null)return {key:'invalid:'+value,label:value};
  if(prefix==="none")return null;
  const length=Number(prefix),shift=BigInt(bits-length),network=(n>>shift)<<shift;
  let address;
  if(bits===32)address=[24n,16n,8n,0n].map(s=>Number((network>>s)&255n)).join('.');
  else {
    const words=Array.from({length:8},(_,i)=>((network>>BigInt((7-i)*16))&65535n).toString(16));
    let best=-1,size=1;
    for(let i=0;i<8;) {if(words[i]!=='0'){i++;continue;}let j=i;while(j<8 && words[j]==='0')j++;if(j-i>size){best=i;size=j-i;}i=j;}
    address=best<0?words.join(':'):words.slice(0,best).join(':')+'::'+words.slice(best+size).join(':');
  }
  return {key:`${bits}:${network}/${length}`,label:address+'/'+length};
}
const api={parse,matches,ipBucket:flowIPBucket};
if(typeof module!=='undefined' && module.exports)module.exports=api;
else if(typeof document==='undefined')root.onmessage=event=>{
 try {
  const {rows,query,regex}=event.data,filter=parse(query,regex),indices=[];
  rows.forEach((row,index)=>{if(matches(row,filter))indices.push(index);});
  root.postMessage({indices});
 }catch(error){root.postMessage({error:error.message});}
};
else root.FlowFilter=api;
})(globalThis);
