// One wasm32 bridge for synchronous browser calls and Promise results. Arguments
// are pointers to typed C values, so large integers and doubles retain their bits.
addToLibrary({
  $h2WebMain__deps: ['$getValue', '$setValue', 'h2_web_main_complete'],
  $h2WebMain: (context, result, completion, types, returnType, callback) => {
    const nativeType = type => ({
      pointer: '*', u16: 'i16', u32: 'i32', u64: 'i64'
    }[type] || type);
    const args = types.map((type, index) => {
      const address = getValue(context + index * 4, '*');
      const value = getValue(address, nativeType(type));
      if (type === 'u16') return value & 65535;
      if (type === 'u32') return value >>> 0;
      if (type === 'u64') return BigInt.asUintN(64, value);
      return value;
    });
    const complete = value => {
      if (returnType !== null) setValue(result, value, nativeType(returnType));
      _h2_web_main_complete(completion);
    };
    const value = callback(...args);
    if (value && typeof value.then === 'function') {
      Promise.resolve(value).then(complete, error => {
        console.error('Browser Promise failed', error);
        complete(-7);
      });
    } else {
      complete(value);
    }
  },
});
