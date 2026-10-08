#include "h2_esp_lierda_command.h"

/* esp_modem 1.4.3 documents this wrapper for C extension developers. Keep
 * that pinned SDK dependency here; neither the public PAL nor BSP sees it.
 * The plain at()/at_raw() C API cannot deliver a per-call collector context
 * with exact, multiple final-result alternatives. */
#include "cxx_include/esp_modem_api.hpp"
#include "esp_private/c_api_wrapper.hpp"

#include <cstring>
#include <new>
#include <string>

namespace {

bool matches(const char *line, size_t length, const char *word) {
    const size_t size = std::strlen(word);
    return length == size && std::memcmp(line, word, size) == 0;
}

bool extended_error(const char *line, size_t length, const char *prefix) {
    const size_t size = std::strlen(prefix);
    return length > size && std::memcmp(line, prefix, size) == 0;
}

int32_t numeric_cme(const char *line, size_t length) {
    size_t pos = std::strlen("+CME ERROR:");
    while (pos < length && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    const size_t first = pos;
    uint32_t value = 0u;
    for (; pos < length && line[pos] >= '0' && line[pos] <= '9'; ++pos) {
        const uint32_t digit = static_cast<uint32_t>(line[pos] - '0');
        if (value > (65535u - digit) / 10u) return -1;
        value = value * 10u + digit;
    }
    if (pos == first) return -1;
    while (pos < length && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    return pos == length ? static_cast<int32_t>(value) : -1;
}

h2_pal_result_t execute(esp_modem_dce_t *dce, const char *command,
    char *response, size_t capacity, uint32_t timeout_ms, h2_esp_lierda_command_status_t *out_status) {
    const std::string wire = std::string(command) + "\r";
    h2_pal_result_t result = H2_PAL_ERR_TIMEOUT;
    out_status->command_started = true;
    const auto sdk_result = static_cast<esp_modem::DCE *>(dce->dce)->command(
        wire, [&](uint8_t *bytes, size_t length) {
            /* DTE callbacks replay the cumulative command buffer, rather than
             * independent chunks. Never append that prefix a second time. */
            if (bytes == nullptr || length == 0u) return esp_modem::command_result::TIMEOUT;
            /* Remember bounds/format failures but keep consuming until the
             * final result. An early FAIL would leave the command's late OK
             * available to a later command. SDK owns its cumulative buffer. */
            if (length >= capacity) result = H2_PAL_ERR_TRUNCATED;
            const char *data = reinterpret_cast<const char *>(bytes);
            if (std::memchr(data, '\0', length) != nullptr &&
                result != H2_PAL_ERR_TRUNCATED) result = H2_PAL_ERR_FORMAT;
            for (size_t start = 0u; start < length;) {
                const char *newline = static_cast<const char *>(
                    std::memchr(data + start, '\n', length - start));
                if (newline == nullptr) break; /* incomplete result is not ACK */
                const size_t end = static_cast<size_t>(newline - data);
                size_t line_size = end - start;
                if (line_size != 0u && data[end - 1u] == '\r') --line_size;
                if (matches(data + start, line_size, "OK")) {
                    out_status->final_result = true;
                    if (result != H2_PAL_ERR_TIMEOUT) return esp_modem::command_result::FAIL;
                    std::memcpy(response, data, start);
                    response[start] = '\0';
                    result = H2_PAL_OK;
                    return esp_modem::command_result::OK;
                }
                const bool cme = extended_error(data + start, line_size, "+CME ERROR:");
                if (matches(data + start, line_size, "ERROR") || cme ||
                    extended_error(data + start, line_size, "+CMS ERROR:")) {
                    out_status->final_result = true;
                    if (cme) out_status->cme_error = numeric_cme(data + start, line_size);
                    if (result == H2_PAL_ERR_TIMEOUT) result = H2_PAL_ERR_IO;
                    return esp_modem::command_result::FAIL;
                }
                start = end + 1u;
            }
            return esp_modem::command_result::TIMEOUT;
        }, timeout_ms);
    out_status->sdk_error = sdk_result == esp_modem::command_result::OK ? ESP_OK
        : sdk_result == esp_modem::command_result::FAIL ? ESP_FAIL : ESP_ERR_TIMEOUT;
    /* SDK may fail on its own receive bound, or time out with partial input.
     * Its synchronous callback removal makes these output writes safe. */
    if (sdk_result == esp_modem::command_result::TIMEOUT) result = H2_PAL_ERR_TIMEOUT;
    else if (result == H2_PAL_ERR_TIMEOUT && sdk_result == esp_modem::command_result::FAIL)
        result = H2_PAL_ERR_IO;
    if (sdk_result != esp_modem::command_result::OK || result != H2_PAL_OK) {
        response[0] = '\0';
        return result;
    }
    return H2_PAL_OK;
}

} // namespace

extern "C" h2_pal_result_t h2_esp_lierda_command(
    esp_modem_dce_t *dce, const char *command, char *response,
    size_t capacity, uint32_t timeout_ms, h2_esp_lierda_command_status_t *out_status) {
    if (response != nullptr && capacity != 0u) response[0] = '\0';
    if (out_status == nullptr) return H2_PAL_ERR_INVALID_ARG;
    *out_status = {ESP_ERR_INVALID_ARG, -1, false, false};
    if (dce == nullptr || dce->dce == nullptr || !dce->dte ||
        dce->modem_type != convert_modem_enum(ESP_MODEM_DCE_GENERIC) ||
        static_cast<int>(dce->dte_type) !=
            static_cast<int>(esp_modem_dce_t::modem_wrap_dte_type::UART) || command == nullptr ||
        response == nullptr || capacity < 2u) return H2_PAL_ERR_INVALID_ARG;
    /* Generated commands are bounded by the provider's APN contract. Reject
     * control/terminator injection before constructing the SDK wire string. */
    size_t length = 0u;
    for (; length < 128u && command[length] != '\0'; ++length) {
        const unsigned char ch = static_cast<unsigned char>(command[length]);
        if (ch < 32u || ch > 126u) return H2_PAL_ERR_INVALID_ARG;
    }
    if (length == 0u || length == 128u) return H2_PAL_ERR_INVALID_ARG;
#ifdef CONFIG_COMPILER_CXX_EXCEPTIONS
    try {
        return execute(dce, command, response, capacity, timeout_ms, out_status);
    } catch (const std::bad_alloc &) {
        response[0] = '\0';
        out_status->sdk_error = ESP_ERR_NO_MEM;
        return H2_PAL_ERR_NO_MEMORY;
    }
#else
    return execute(dce, command, response, capacity, timeout_ms, out_status);
#endif
}
