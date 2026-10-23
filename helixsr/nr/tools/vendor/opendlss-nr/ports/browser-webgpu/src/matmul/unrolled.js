// Make the native 4/8/16-term group bounds explicit before WGSL compilation.
// Operand loads and every multiply/truncation/add retain their original order.
export function unrolledMatmulCode(code) {
  const begin=code.indexOf('fn quad_fdpa('), end=code.indexOf('\n}',begin)+2;
  if(begin<0 || end<2)throw new Error('Missing quad reduction');
  const original=code.slice(begin,end), functions=[];
  for(const count of [4,8,16])for(let start=0;start<32;start+=count){
    let body=original.replace('fn quad_fdpa(',`fn quad_fdpa_${count}_${start}(`)
      .replace('start: u32, count: u32, ','');
    const loop='  for (var k = start / 4u; k < (start + count) / 4u; k++) {';
    while(body.includes(loop)){
      const a=body.indexOf(loop), b=body.indexOf('\n  }',a)+4;
      if(b<4)throw new Error('Missing matrix reduction loop end');
      const terms=body.slice(a+loop.length,b-4);
      body=body.slice(0,a)+Array.from({length:count/4},(_,i)=>
        `  {\n    let k = ${start/4+i}u;${terms}\n  }`).join('\n')+body.slice(b);
    }
    functions.push(body);
  }
  code=code.slice(0,begin)+functions.join('\n')+code.slice(end);
  const a=code.indexOf('    var length = 16u;'),b=code.indexOf('    sums[0] = quad.x;',a);
  if(a<0 || b<0)throw new Error('Missing native group dispatch');
  const calls=count=>Array.from({length:32/count},(_,i)=>
    `      quad = quad_fdpa_${count}_${i*count}(local_id.y * 2u, local_id.x * 2u, quad);`).join('\n');
  return code.slice(0,a)+`    if ((MATMUL_FLAGS & 2048u) != 0u) {\n${calls(8)}
    } else if ((MATMUL_FLAGS & 4096u) != 0u) {\n${calls(4)}
    } else {\n${calls(16)}
    }
`+code.slice(b);
}
