/* Offline sculpt for the six plated servings. Build with cc -O3 -std=c11
 * sculpt.c -lm -o sculpt. The camera, lathe and light are shared by all six.
 * World coordinates are in plate radii. Output is an RGBA byte stream. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define PI 3.14159265f

typedef struct {
  float x, y, z;
} v3;
typedef struct {
  float d;
  int mat;
  v3 uv;
} hit;
enum {
  CLAY,
  RICE,
  SALMON,
  EGG,
  TUNA,
  SHRIMP,
  TAIL,
  NORI,
  CUCUMBER,
  INARI,
  LEAF,
  GINGER,
  SEED
};
static int dish;
static v3 v(float x, float y, float z) { return (v3){x, y, z}; }
static v3 add(v3 a, v3 b) { return v(a.x + b.x, a.y + b.y, a.z + b.z); }
static v3 mul(v3 a, float s) { return v(a.x * s, a.y * s, a.z * s); }
static float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 unit(v3 a) { return mul(a, 1.f / sqrtf(dot(a, a))); }
static float sat(float a) { return fminf(1.f, fmaxf(0.f, a)); }
static float len2(float a, float b) { return sqrtf(a * a + b * b); }
static v3 mix(v3 a, v3 b, float t) { return add(mul(a, 1 - t), mul(b, t)); }
static float box(v3 p, v3 half, float r) {
  float a = fabsf(p.x) - half.x, b = fabsf(p.y) - half.y,
        c = fabsf(p.z) - half.z;
  return sqrtf(powf(fmaxf(a, 0), 2) + powf(fmaxf(b, 0), 2) +
               powf(fmaxf(c, 0), 2)) +
         fminf(fmaxf(a, fmaxf(b, c)), 0) - r;
}
static float ell(v3 p, float x, float y, float z) {
  return (sqrtf((p.x / x) * (p.x / x) + (p.y / y) * (p.y / y) +
                (p.z / z) * (p.z / z)) -
          1) *
         fminf(x, fminf(y, z));
}
static float bun(v3 p, float x, float y, float z) {
  float a = p.x / x, b = p.y / y, c = p.z / z;
  return (sqrtf(sqrtf(a * a * a * a + b * b * b * b + c * c * c * c)) - 1) *
         fminf(x, fminf(y, z));
}
static void put(hit* h, float d, int mat, v3 uv) {
  if (d < h->d) *h = (hit){d, mat, uv};
}
static float hash(int x, int y, int z) {
  uint32_t n = (uint32_t)x * 0x1f1f1f1fu ^ (uint32_t)y * 0x8da6b343u ^
               (uint32_t)z * 0xd8163841u;
  n ^= n >> 15;
  n *= 0x85ebca6bu;
  return (n >> 8) / 16777216.f;
}
static float grid(v3 p, float f) {
  return hash((int)floorf(p.x * f), (int)floorf(p.y * f), (int)floorf(p.z * f));
}
/* A bent fish fillet drapes over the compressed rice. The short end is
 * narrower and thinner, so the two pieces do not read as rectangular tiles. */
static void nigiri(hit* h, v3 p, int piece) {
  float taper = 1 - .22f * sat((p.x + .45f) / 1.f);
  float warp = .035f * sinf(p.x * 4 + piece);
  v3 r = v(p.x, p.y - warp, p.z - .318f);
  if (dish != 5) {
    float rough =
        .008f * sinf(29 * r.x + 12 * r.z) * sinf(43 * r.z + 19 * r.y) +
        .004f * sinf(71 * r.y + 52 * r.x);
    put(h, bun(r, .50f, .211f, .174f) + rough, RICE, r);
  }
  float drop = .18f * p.x * p.x + .73f * p.y * p.y + .017f * sinf(6 * p.x + 1);
  v3 t = v(p.x, (p.y - warp) / taper, p.z - .484f + drop);
  if (dish == 0 || dish == 2) {
    float endroll = .10f * sat((-p.x - .22f) * 3) * sat((-p.x - .22f) * 3);
    v3 mantle = v(t.x, t.y, t.z + endroll);
    put(h, bun(mantle, .59f, .274f, .107f), dish == 0 ? SALMON : TUNA, mantle);
  } else if (dish == 1) {
    put(h, box(t, v(.42f, .158f, .048f), .066f), EGG, t);
    v3 n = v(p.x + .02f, t.y, p.z - .369f + drop * .4f);
    put(h, box(n, v(.075f, .162f, .14f), .076f), NORI, n);
  } else if (dish == 3) {
    float along = sat((p.x + .56f) / 1.12f), belly = 1 - .34f * along;
    float ring = .5f + .5f * cosf(along * 32.f + 3.5f * p.y);
    v3 flesh =
        v(p.x + .01f,
          (p.y - .066f * sinf(along * PI)) / (belly * (1 - .08f * ring * ring)),
          p.z - .471f + .058f * along - .025f * sinf(along * PI));
    put(h, bun(flesh, .57f, .237f, .119f), SHRIMP, v(along, p.y, flesh.z));
    for (int i = -1; i <= 1; i++) {
      float a = i * .49f, c = cosf(a), s = sinf(a), x = p.x - .49f,
            y = p.y - i * .064f;
      v3 q = v(x * c + y * s, -x * s + y * c, p.z - .455f - x * .22f);
      put(h, ell(q, .215f, .078f, .035f), TAIL, q);
    }
  } else if (dish == 5) {
    v3 bag = v(p.x, p.y, p.z - .37f);
    float lip = .018f * sinf(p.x * 22 + piece) + .008f * sinf(p.x * 48);
    float outer = box(bag, v(.315f, .089f, .10f), .158f);
    v3 mouth = v(p.x, p.y, p.z - .64f - lip);
    float hollow = ell(mouth, .398f, .177f, .13f);
    put(h, fmaxf(outer, -hollow), INARI, bag);
    put(h, ell(v(p.x, p.y, p.z - .515f), .354f, .149f, .071f), RICE, p);
    for (int i = 0; i < 4; i++) {
      float a = (i - 1.7f) * .126f, b = .054f * sinf(i * 4 + piece);
      put(h,
          ell(v(p.x - a, p.y - b, p.z - .577f + .06f * a * a), .032f, .016f,
              .016f),
          SEED, p);
    }
  }
}
static hit scene(v3 p) {
  float radius = len2(p.x, p.y), r = radius / 1.32f;
  /* Thrown plate: foot ring, sloping well, rolled lip and glazed interior. */
  float z = .106f + .112f * r * r * r;
  float outer = fmaxf(radius - 1.318f, fabsf(p.z - z) - .043f);
  float lip = len2(radius - 1.29f, p.z - .207f) - .041f;
  hit h = {fminf(fminf(outer, lip), len2(radius - .81f, p.z - .059f) - .044f),
           CLAY, p};
  if (dish >= 6) return h;
  if (dish == 4) {
    const float xs[] = {-.42f, .27f, .03f}, ys[] = {.10f, .22f, -.36f};
    for (int i = 0; i < 3; i++) {
      v3 q = v(p.x - xs[i], p.y - ys[i], p.z - .335f);
      float d = len2(q.x, q.y);
      put(&h, fmaxf(d - .300f, fabsf(q.z) - .200f), NORI, q);
      put(&h, fmaxf(d - .273f, fabsf(q.z) - .205f), RICE, q);
      v3 seed = v(q.x, q.y, q.z - .185f);
      put(&h, box(seed, v(.068f, .080f, .002f), .024f), CUCUMBER, q);
    }
  } else {
    for (int i = 0; i < 2; i++) {
      float x = p.x + (i ? .17f : -.17f), y = p.y + (i ? .29f : -.26f),
            a = i ? .42f : .53f;
      v3 q = v(x * cosf(a) + y * sinf(a), -x * sinf(a) + y * cosf(a), p.z);
      nigiri(&h, q, i);
    }
  }
  /* Shiso under the far piece, with saw teeth cut into its outline. */
  v3 l = v(p.x + .37f, p.y - .36f, p.z - .208f);
  float lx = l.x * .80f + l.y * .60f, ly = l.y * .80f - l.x * .60f;
  float lw = .295f * sat(1 - fabsf(lx / .67f)) + .018f * sinf(lx * 86);
  put(&h,
      fmaxf(fabsf(l.z - .12f * ly * ly - .08f * lx * lx) - .012f,
            fmaxf(fabsf(ly) - lw, fabsf(lx) - .66f)),
      LEAF, v(lx, ly, l.z));
  for (int i = 0; i < 3; i++) {
    v3 q = v(p.x - .75f + .07f * i, p.y + .32f + .037f * i,
             p.z - .20f - i * .025f);
    q.z += .18f * q.y + .018f * sinf(q.x * 20 + i);
    put(&h, ell(q, .15f, .118f, .029f), GINGER, q);
  }
  return h;
}
static v3 pigment(hit h, v3 p) {
  v3 q = h.uv;
  float g = grid(p, 115), f = grid(p, 45);
  v3 c;
  switch (h.mat) {
    case CLAY: {
      float r = len2(p.x, p.y), a = atan2f(p.y, p.x);
      v3 glazes[] = {{0.54, .80, .77}, {.83, .86, .76}, {.53, .72, .81},
                     {.84, .82, .68},  {.65, .81, .69}, {.69, .72, .83},
                     {.56, .78, .74}};
      c = glazes[dish > 6 ? dish - 7 : dish];
      if ((r > 1.19f && r < 1.209f) || (r > 1.256f && r < 1.272f))
        c = mul(c, .58f);
      if (r > 1.12f && r < 1.158f && cosf(a * 32 + (r - 1.12f) * 42) > .22f)
        c = mix(c, v(.12, .38, .43), .72f);
      if (r > .89f && r < .902f) c = mul(c, .81f);
      if (f > .983f) c = mul(c, .86f);
      break;
    }
    case RICE: {
      float x = q.x * 26 + q.y * 5 + q.z * 9, y = q.y * 33 + q.z * 32;
      int ix = (int)floorf(x), iy = (int)floorf(y);
      float grain = 4, shade = 1;
      for (int j = iy - 1; j <= iy + 1; j++)
        for (int i = ix - 1; i <= ix + 1; i++) {
          float seed = hash(i, j, 17), dx = x - (i + .15f + .70f * seed),
                dy = y - (j + .15f + .70f * hash(i, j, 33));
          float tilt = seed * .9f - .45f, ax = dx + tilt * dy,
                ay = dy - tilt * dx;
          float k = ax * ax * .82f + ay * ay * 1.45f;
          if (k < grain) {
            grain = k;
            shade = seed;
          }
        }
      c = mix(v(.76, .72, .60), v(1, .988, .879),
              sat(1.08f - grain * .80f - .11f * shade));
      break;
    }
    case SALMON:
    case TUNA: {
      float band = fabsf(sinf(13 * q.x + 8 * q.y +
                              2.05f * sinf(q.y * 6 + q.x * 1.3f) + q.z * 6));
      float seam = sat((.43f - band) * 3.2f);
      c = h.mat == SALMON
              ? mix(v(.98, .48, .29), v(1, .845, .64), seam)
              : mix(v(.77, .14, .24), v(.96, .49, .52), seam * .48f);
      if (h.mat == SALMON) {
        float sear = fabsf(sinf(q.x * 12 - q.y * 4 + .34f * sinf(q.y * 13)));
        float ash =
            sat((.34f - sear) * 4) * sat((q.z + .02f) * 11) * (.42f + .48f * f);
        c = mix(c, v(.41, .18, .079), ash);
      }
      break;
    }
    case EGG:
      c = mix(v(.89, .558, .094), v(.998, .811, .296),
              .52f + .48f * sinf(q.z * 101 + q.y * 10 + 1.5f * sinf(q.x * 4)));
      if (f > .94f) c = mul(c, .9f);
      break;
    case SHRIMP: {
      float stripe = .5f + .5f * cosf(q.x * 31.f + 4 * q.y - 3.8f * q.z +
                                      1.1f * sinf(q.y * 11));
      c = mix(v(.994, .914, .806), v(.93, .415, .254),
              sat((stripe - .24f) * 1.72f) * (.67f + .27f * q.x));
      break;
    }
    case TAIL:
      c = mix(v(.85, .24, .115), v(1, .64, .29), .5f + .5f * cosf(q.y * 96));
      break;
    case NORI:
      c = mix(v(.066, .144, .14), v(.19, .284, .218),
              .28f * g + .30f * f + .23f * (1 + cosf(p.z * 64 + p.x * 12)));
      break;
    case CUCUMBER:
      c = mix(v(.15, .53, .19), v(.66, .88, .36),
              sat(1.3f - len2(q.x * 10, q.y * 12)));
      break;
    case INARI:
      c = mix(v(.61, .294, .081), v(.94, .614, .257),
              .57f + .15f * sinf(q.x * 40 + q.y * 53 + 2 * sinf(q.z * 37)) +
                  .25f * f);
      break;
    case LEAF:
      c = mix(v(.16, .41, .246), v(.48, .65, .32),
              sat((.011f - fabsf(q.y)) * 85) +
                  .3f * sat((cosf(q.x * 40 - fabsf(q.y) * 35) - .91f) * 12));
      break;
    case GINGER:
      c = v(.98, .70, .61);
      break;
    default:
      c = v(.38, .22, .13);
      break;
  }
  return mul(c, .986f + .027f * g);
}
static float shadow(v3 p, v3 dir) {
  float t = .012f, res = 1;
  for (int i = 0; i < 32 && t < 1.5f; i++) {
    float d = scene(add(p, mul(dir, t))).d;
    res = fminf(res, 8.f * d / t);
    if (res < .04f) break;
    t += fmaxf(.012f, d * .9f);
  }
  return sat(res);
}
int main(int argc, char** argv) {
  if (argc != 4) return 1;
  dish = atoi(argv[1]);
  int w = atoi(argv[2]), h = atoi(argv[3]);
  if (dish < 0 || dish > 12 || w <= 0 || h <= 0) return 2;
  v3 eye = unit(v(0, -.82f, .62f)), up = v(0, eye.z, -eye.y),
     ray = mul(eye, -1), light = unit(v(-.42f, -.43f, .81f));
  v3 half = unit(add(eye, light));
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      float sx = (x + .5f - w / 2.f) * 2.91f / w,
            sy = (h * .53f - y - .5f) * 2.91f / w;
      v3 o = add(v(sx, 0, .19f), add(mul(up, sy), mul(eye, 3)));
      float t = 1.15f;
      hit hitp;
      v3 p;
      int ok = 0;
      for (int i = 0; i < 105 && t < 4.65f; i++) {
        p = add(o, mul(ray, t));
        hitp = scene(p);
        if (hitp.d < .0011f) {
          ok = 1;
          break;
        }
        t += fmaxf(.002f, hitp.d * .86f);
      }
      unsigned char rgba[4] = {0};
      if (ok) {
        float e = .002f;
        v3 n =
            unit(v(scene(add(p, v(e, 0, 0))).d - scene(add(p, v(-e, 0, 0))).d,
                   scene(add(p, v(0, e, 0))).d - scene(add(p, v(0, -e, 0))).d,
                   scene(add(p, v(0, 0, e))).d - scene(add(p, v(0, 0, -e))).d));
        float l = sat(dot(n, light)),
              s = l > 0 ? shadow(add(p, mul(n, .008f)), light) : 0;
        float ao = sat(scene(add(p, mul(n, .12f))).d / .12f);
        float spec = powf(sat(dot(n, half)), hitp.mat == CLAY ? 54 : 25) *
                     (hitp.mat == CLAY ? .52f : .23f) * s;
        float lum =
            .64f + .15f * n.z + .32f * l * (.2f + .8f * s) - .10f * (1 - ao);
        v3 c = mul(pigment(hitp, p), lum);
        c = add(c, v(spec, spec * .92f, spec * .78f));
        rgba[0] = (uint8_t)(255 * sat(c.x));
        rgba[1] = (uint8_t)(255 * sat(c.y));
        rgba[2] = (uint8_t)(255 * sat(c.z));
        rgba[3] = 255;
      }
      if (fwrite(rgba, 1, 4, stdout) != 4) return 3;
    }
  return 0;
}
