// Expose the fixed native 16-term groups to the compiler without changing
// exponent selection, product truncation, addition order or half publication.
export function unrolledWindowCode(code) {
  for (const [name, starts] of [['tiled_qk', [0,16]], ['tiled_value', [0,16,32,48]]]) {
    const begin=code.indexOf(`fn ${name}(`), end=code.indexOf('\n}',begin)+2;
    if(begin<0 || end<2)throw new Error(`Missing ${name}`);
    const original=code.slice(begin,end), specialized=[];
    for(const start of starts){
      let body=original.replace(`fn ${name}(`,`fn ${name}_${start}(`)
        .replace('start: u32, ','');
      const loop='  for (var c = start / 4u; c < (start + 16u) / 4u; c++) {';
      while(body.includes(loop)){
        const a=body.indexOf(loop), b=body.indexOf('\n  }',a)+4;
        if(b<4)throw new Error('Missing window reduction loop end');
        const terms=body.slice(a+loop.length,b-4);
        body=body.slice(0,a)+Array.from({length:4},(_,i)=>
          `  {\n    let c = ${start/4+i}u;${terms}\n  }`).join('\n')+body.slice(b);
      }
      specialized.push(body);
    }
    const other=name==='tiled_qk'?'key':'component';
    const dispatch=`fn ${name}(query:u32, ${other}:u32, start:u32, accumulator:f32)->f32 {\n`
      + starts.slice(0,-1).map(s=>`  if(start==${s}u){return ${name}_${s}(query,${other},accumulator);}`).join('\n')
      + `\n  return ${name}_${starts.at(-1)}(query,${other},accumulator);\n}`;
    code=code.slice(0,begin)+specialized.join('\n')+'\n'+dispatch+code.slice(end);
  }
  return code;
}
