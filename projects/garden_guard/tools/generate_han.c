/* Regenerates the checked-in bitmap from the pinned AOSP Droid Sans Fallback
 * font. Usage: generate_han FONT UTF8_CATALOG_SOURCE > h2_garden_guard_zh.inc
 */
#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HAN_SIDE 14
#define HAN_SMALL_SIDE 12
#define BMP_COUNT 65536u

static bool rasterize(FT_Face face, unsigned code, unsigned side,
                      FT_Int32 flags, uint32_t* rows) {
  if (FT_Set_Pixel_Sizes(face, 0, side) ||
      FT_Load_Char(face, code, FT_LOAD_RENDER | flags))
    return false;
  const FT_Bitmap* bitmap = &face->glyph->bitmap;
  if (bitmap->width > side || bitmap->rows > side ||
      bitmap->pixel_mode != FT_PIXEL_MODE_GRAY || bitmap->pitch < 0)
    return false;
  /* Match punctuation and ideographs to the same baseline in each cell. */
  const int baseline = side == HAN_SMALL_SIDE ? 10 : 12;
  int top = baseline - face->glyph->bitmap_top;
  if (top < 0) top = 0;
  if (top + (int)bitmap->rows > (int)side) top = (int)(side - bitmap->rows);
  int left = face->glyph->bitmap_left;
  if (left < 0) left = 0;
  if (left + (int)bitmap->width > (int)side) left = (int)(side - bitmap->width);
  for (unsigned y = 0; y < bitmap->rows; ++y)
    for (unsigned x = 0; x < bitmap->width; ++x) {
      const unsigned alpha = bitmap->buffer[y * (unsigned)bitmap->pitch + x];
      /* Preserve faint strokes at native size without making the antialias
       * fringe opaque. The quiet larger master keeps linear coverage. */
      const unsigned coverage = side == HAN_SMALL_SIDE
                                    ? (alpha >= 198u   ? 3u
                                       : alpha >= 109u ? 2u
                                       : alpha >= 32u  ? 1u
                                                       : 0u)
                                    : (alpha * 3u + 127u) / 255u;
      rows[(unsigned)top + y] |= coverage << (2u * ((unsigned)left + x));
    }
  return true;
}

static void print_rows(const uint32_t* rows, unsigned side) {
  for (unsigned y = 0; y < side; ++y) {
    if (y != 0) fputs(y % 5u == 0 ? ",\n               " : ", ", stdout);
    printf("0x%08lxu", (unsigned long)rows[y]);
  }
}

static void print_small_rows(const uint32_t* rows) {
  for (unsigned y = 0; y < HAN_SMALL_SIDE; ++y) {
    if (y != 0u) fputs(y % 3u == 0u ? ",\n               " : ", ", stdout);
    printf("{0x%02xu, 0x%02xu, 0x%02xu}", (unsigned)(rows[y] & 255u),
           (unsigned)((rows[y] >> 8u) & 255u), (unsigned)(rows[y] >> 16u));
  }
}

/* The cover title and controls use their own 4-bit master bitmaps. */
static bool print_cover(FT_Face face, const char* name, unsigned side,
                        const unsigned* glyphs, size_t count) {
  printf("static const uint8_t %s[%zu][%u] = {\n", name, count,
         side * side / 2u);
  for (size_t i = 0; i < count; ++i) {
    if (FT_Set_Pixel_Sizes(face, 0, side) ||
        FT_Load_Char(face, glyphs[i], FT_LOAD_RENDER | FT_LOAD_NO_HINTING))
      return false;
    const FT_Bitmap* b = &face->glyph->bitmap;
    if (b->width > side || b->rows > side || b->pitch < 0 ||
        b->pixel_mode != FT_PIXEL_MODE_GRAY)
      return false;
    const unsigned top = (side - b->rows) / 2u;
    const unsigned left = (side - b->width) / 2u;
    uint8_t packed[32u * 32u / 2u] = {0};
    for (unsigned y = 0; y < b->rows; ++y) {
      for (unsigned x = 0; x < b->width; ++x) {
        const unsigned pixel = (top + y) * side + left + x;
        const unsigned coverage =
            (b->buffer[y * (unsigned)b->pitch + x] * 15u + 127u) / 255u;
        packed[pixel / 2u] |= (uint8_t)(coverage << ((pixel & 1u) * 4u));
      }
    }
    printf("    { /* U+%04X */\n", glyphs[i]);
    for (unsigned j = 0; j < side * side / 2u; ++j) {
      if (j % 12u == 0u) fputs("      ", stdout);
      printf("0x%02xu,", (unsigned)packed[j]);
      if (j % 12u == 11u || j + 1u == side * side / 2u)
        putchar('\n');
      else
        putchar(' ');
    }
    puts("    },");
  }
  puts("};");
  return true;
}

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  FILE* input = fopen(argv[2], "rb");
  if (input == NULL) return 2;
  static bool used[BMP_COUNT];
  int byte;
  while ((byte = fgetc(input)) != EOF) {
    if (byte < 128) continue;
    const int second = fgetc(input);
    const int third = fgetc(input);
    if (byte < 0xe0 || byte > 0xef || second < 0x80 || second > 0xbf ||
        third < 0x80 || third > 0xbf) {
      fclose(input);
      fprintf(stderr, "Expected three-byte UTF-8 catalog characters\n");
      return 2;
    }
    used[((unsigned)byte & 15u) << 12 | ((unsigned)second & 63u) << 6 |
         ((unsigned)third & 63u)] = true;
  }
  fclose(input);
  FT_Library library;
  FT_Face face;
  if (FT_Init_FreeType(&library) || FT_New_Face(library, argv[1], 0, &face))
    return 2;

  const char* postscript = FT_Get_Postscript_Name(face);
  if (postscript == NULL || strcmp(postscript, "DroidSansFallback") != 0 ||
      FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) {
    fprintf(stderr, "Expected a Unicode Droid Sans Fallback face\n");
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return EXIT_FAILURE;
  }

  puts(
      "/* Generated by tools/generate_han.c. Source: Droid Sans Fallback 2.55b.\n"
      " * Apache-2.0, see targets/pkg_tar/web/droid-sans-fallback-notice.txt. */\n"
      "#define HAN_SIDE 14\n"
      "#define HAN_SMALL_SIDE 12\n"
      "static const struct {\n"
      "  uint16_t code;\n"
      "  uint32_t rows[HAN_SIDE];\n"
      "  uint8_t small[HAN_SMALL_SIDE][3];\n"
      "} k_han[] = {");

  for (unsigned code = 128u; code < BMP_COUNT; ++code) {
    if (!used[code]) continue;
    uint32_t rows[HAN_SIDE] = {0};
    uint32_t small[HAN_SMALL_SIDE] = {0};
    if (FT_Get_Char_Index(face, code) == 0 ||
        !rasterize(face, code, HAN_SIDE, FT_LOAD_NO_HINTING, rows) ||
        !rasterize(face, code, HAN_SMALL_SIDE,
                   FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_LIGHT, small)) {
      fprintf(stderr, "Missing or oversized font glyph U+%04X\n", code);
      return 1;
    }
    printf("    {0x%04xu, {", code);
    print_rows(rows, HAN_SIDE);
    fputs("},\n              {", stdout);
    print_small_rows(small);
    puts("}},");
  }
  puts("};");
  static const unsigned title[] = {0x690du, 0x7269u, 0x5b88u, 0x536bu};
  static const unsigned start[] = {0x5f00u, 0x59cbu, 0x6e38u, 0x620fu};
  const bool cover = print_cover(face, "k_cover_title", 32u, title, 4u) &&
                     print_cover(face, "k_cover_start", 18u, start, 4u);
  FT_Done_Face(face);
  FT_Done_FreeType(library);
  return !cover || ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
}
