/* Intentionally unguarded: configure and instantiate once per precision.
 * All floating-point arithmetic in the f32 instance stays in float. Explicit
 * conversion to/from Lua numbers happens only at the API boundary. */
#undef NUM_REAL
#undef NUM_NAME
#undef NUM_MATH
#undef NUM_C
#undef NUM_TINY_SQUARED
#undef NUM_DATA
#if H2_NUMERIC_F32
#define NUM_DATA(b) ((b)->data.f32)
#define NUM_REAL float
#define NUM_NAME(name) name##_f32
#define NUM_MATH(name) name##f
#define NUM_C(value) value##f
#define NUM_TINY_SQUARED 1e-30f
#else
#define NUM_DATA(b) ((b)->data.f64)
#define NUM_REAL double
#define NUM_NAME(name) name##_f64
#define NUM_MATH(name) name
#define NUM_C(value) value
#define NUM_TINY_SQUARED 1e-280
#endif

static h2_numeric_buffer_t *NUM_NAME(buffer_check)(lua_State *s, int at) {
  h2_numeric_buffer_t *b = h2_numeric_check(s, at);
  if (b->is_f32 != H2_NUMERIC_F32)
    luaL_error(s, "mixed numeric buffer kinds (f32/f64)");
  return b;
}
static NUM_REAL NUM_NAME(checked)(lua_State *s, NUM_REAL value) {
  if (!isfinite(value) || NUM_MATH(fabs)(value) > NUM_C(1000000.0))
    luaL_error(s, "numeric value outside finite bounds");
  return value;
}
static NUM_REAL NUM_NAME(number)(lua_State *s, int at) {
  /* Validate the original Lua number so rounding cannot admit out-of-range
   * input. Conversion is once per scalar, outside all numeric loops. */
  return (NUM_REAL)h2_numeric_number(s, at);
}
static void NUM_NAME(commit)(lua_State *s, h2_numeric_buffer_t *b, size_t n) {
  NUM_REAL *work = NUM_DATA(b) + b->count;
  for (size_t i = 0; i < n; ++i)
    NUM_NAME(checked)(s, work[i]);
  memcpy(NUM_DATA(b), work, n * sizeof(NUM_REAL));
}
