#ifndef LIERDA_TEST_C_API_WRAPPER_HPP
#define LIERDA_TEST_C_API_WRAPPER_HPP

#include "lierda_test_sdk.h"
#include <functional>
#include <string>

namespace esp_modem {
enum class command_result { OK, FAIL, TIMEOUT };
using got_line_cb = std::function<command_result(uint8_t *, size_t)>;
class DCE {
public:
    command_result command(const std::string &wire, got_line_cb callback, uint32_t timeout);
};
}
inline int convert_modem_enum(int module) { return module; }
#endif
