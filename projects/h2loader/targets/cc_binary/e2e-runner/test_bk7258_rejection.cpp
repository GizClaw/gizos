#include "bk7258_rejection.h"

#include <cassert>
#include <string>

int main() {
  const std::string proof =
      "H2_BK_FLASH_REJECTION valid=1 aes=1 rc=-3 erase_calls=0 write_calls=0\n";
  auto check = [](const std::string& text) {
    return h2_bk7258_e2e_check_rejection(nullptr, text.data(), text.size());
  };
  assert(check(proof) == H2_PAL_OK);
  assert(check("stats header\n" + proof + "stats tail\r\n") == H2_PAL_OK);
  assert(check(proof.substr(0, proof.size() - 1) + "\r\n") == H2_PAL_OK);
  assert(check("") != H2_PAL_OK);
  assert(check(proof + proof) != H2_PAL_OK);
  assert(check("prefix " + proof) != H2_PAL_OK);
  assert(check(proof.substr(0, proof.size() - 1)) != H2_PAL_OK);
  for (const std::string field :
       {"valid=1", "aes=1", "rc=-3", "erase_calls=0", "write_calls=0"}) {
    auto bad = proof;
    bad.replace(bad.find(field), field.size(), field + "9");
    assert(check(bad) != H2_PAL_OK);
  }
  // A valid prefix cannot conceal contradictory or truncated fields.
  assert(check(proof.substr(0, proof.size() - 1) + " extra=0\n") != H2_PAL_OK);
  assert(h2_bk7258_e2e_check_rejection(nullptr, nullptr, 0) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk7258_e2e_check_rejection(nullptr, proof.data(),
                                       proof.size() - 2) != H2_PAL_OK);
}
