#include "h2_g99_e2e_internal.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

static void test_selected(void) {
  CHECK(h2_g99_e2e_selected(NULL, "startup", "menu-ready"));
  CHECK(h2_g99_e2e_selected("", "startup", "menu-ready"));
  CHECK(h2_g99_e2e_selected("startup", "startup", "menu-ready"));
  CHECK(!h2_g99_e2e_selected("startup", "keys", "screen-toggle"));
  CHECK(h2_g99_e2e_selected("keys/screen-toggle", "keys", "screen-toggle"));
  CHECK(!h2_g99_e2e_selected("keys/screen", "keys", "screen-toggle"));
  CHECK(h2_g99_e2e_selected("keys/screen*", "keys", "screen-toggle"));
  CHECK(h2_g99_e2e_selected("nav*", "navigation", "non-enterable"));
  CHECK(!h2_g99_e2e_selected("-wifi", "wifi", "connect-test-network"));
  CHECK(h2_g99_e2e_selected("-wifi", "keys", "screen-toggle"));
  CHECK(!h2_g99_e2e_selected("keys, -keys/screen-toggle", "keys",
                             "screen-toggle"));
  CHECK(h2_g99_e2e_selected(" keys , settings ", "settings",
                            "persist-across-restart"));
  /* A group name is not a prefix of another group. */
  CHECK(!h2_g99_e2e_selected("key", "keys", "screen-toggle"));
}

static void test_valid(void) {
  char term[32];
  CHECK(h2_g99_e2e_select_valid(NULL, term, sizeof(term)));
  CHECK(h2_g99_e2e_select_valid("", term, sizeof(term)));
  CHECK(h2_g99_e2e_select_valid("startup,navigation/non-enterable,-wifi", term,
                                sizeof(term)));
  CHECK(h2_g99_e2e_select_valid("layout/*", term, sizeof(term)));
  CHECK(!h2_g99_e2e_select_valid("startup,bogus", term, sizeof(term)));
  CHECK(strcmp(term, "bogus") == 0);
  CHECK(!h2_g99_e2e_select_valid("-keys/nope", term, sizeof(term)));
  CHECK(strcmp(term, "keys/nope") == 0);
  char longest[H2_G99_E2E_SELECT_MAX + 2u];
  memset(longest, 'a', sizeof(longest) - 1u);
  longest[sizeof(longest) - 1u] = '\0';
  CHECK(!h2_g99_e2e_select_valid(longest, term, sizeof(term)));
}

int main(void) {
  test_selected();
  test_valid();
  if (failures != 0) return 1;
  (void)printf("PASS\n");
  return 0;
}
