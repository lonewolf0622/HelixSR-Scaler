export const WINDOW_LAYOUT_FIELDS = ['width','height','channels','shift_x','shift_y',
  'use_relative_bias','value_group_order'];

// These values are immutable for a captured geometry. Pipeline constants let
// the compiler fold address divisions and the native value-group permutation.
export function layoutWindowCode(code) {
  let declarations='';
  for(const field of WINDOW_LAYOUT_FIELDS){
    const constant=`WINDOW_${field.toUpperCase()}`;
    declarations+=`override ${constant}:u32=0u;\n`;
    code=code.replaceAll(`attention_params.${field}`,constant);
  }
  return code.replace('struct WindowParams',declarations+'struct WindowParams');
}
