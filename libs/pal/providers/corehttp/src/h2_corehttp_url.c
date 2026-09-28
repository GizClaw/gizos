#include "h2_corehttp_internal.h"

#include <limits.h>
#include <string.h>

static bool ascii_equal_ci(char left, char right) {
    if (left >= 'A' && left <= 'Z') {
        left = (char)(left - 'A' + 'a');
    }
    if (right >= 'A' && right <= 'Z') {
        right = (char)(right - 'A' + 'a');
    }
    return left == right;
}

static bool span_equal_ci(const char *data, size_t len, const char *literal) {
    size_t literal_len = strlen(literal);
    if (data == NULL || len != literal_len) {
        return false;
    }
    for (size_t index = 0u; index < len; ++index) {
        if (!ascii_equal_ci(data[index], literal[index])) {
            return false;
        }
    }
    return true;
}

static bool has_uri_scheme(const char *data, size_t len) {
    if (data == NULL || len < 2u ||
        !((data[0] >= 'A' && data[0] <= 'Z') ||
          (data[0] >= 'a' && data[0] <= 'z'))) {
        return false;
    }
    for (size_t index = 1u; index < len; ++index) {
        char byte = data[index];
        if (byte == ':') {
            return true;
        }
        if (!((byte >= 'A' && byte <= 'Z') ||
              (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '+' || byte == '-' ||
              byte == '.')) {
            return false;
        }
    }
    return false;
}

static char *duplicate_span(
    const h2_pal_mem_api_t *allocator,
    const char *data,
    size_t len) {
    if (data == NULL || len == SIZE_MAX) {
        return NULL;
    }
    char *copy = (char *)h2_pal_mem_alloc(allocator, len + 1u);
    if (copy == NULL) {
        return NULL;
    }
    if (len > 0u) {
        memcpy(copy, data, len);
    }
    copy[len] = '\0';
    return copy;
}

static h2_pal_result_t parse_port(
    const char *data,
    size_t len,
    uint16_t *out_port) {
    if (data == NULL || len == 0u || out_port == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint32_t port = 0u;
    for (size_t index = 0u; index < len; ++index) {
        if (data[index] < '0' || data[index] > '9') {
            return H2_PAL_ERR_INVALID_ARG;
        }
        uint32_t digit = (uint32_t)(data[index] - '0');
        if (port > (UINT16_MAX - digit) / 10u) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        port = port * 10u + digit;
    }
    if (port == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_port = (uint16_t)port;
    return H2_PAL_OK;
}

void h2_corehttp_url_deinit(h2_corehttp_t *provider, h2_corehttp_url_t *url) {
    if (provider == NULL || url == NULL) {
        return;
    }
    h2_pal_mem_free(provider->config.allocator, url->host);
    h2_pal_mem_free(provider->config.allocator, url->authority);
    h2_pal_mem_free(provider->config.allocator, url->path);
    memset(url, 0, sizeof(*url));
}

h2_pal_result_t h2_corehttp_parse_url(
    h2_corehttp_t *provider,
    const char *data,
    size_t len,
    h2_corehttp_url_t *out_url) {
    if (provider == NULL || data == NULL || len == 0u || out_url == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_url, 0, sizeof(*out_url));
    for (size_t index = 0u; index < len; ++index) {
        unsigned char byte = (unsigned char)data[index];
        if (byte <= 0x20u || byte == 0x7fu || byte == '#') {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }

    size_t scheme_len = 0u;
    bool secure = false;
    if (len >= 7u && span_equal_ci(data, 7u, "http://")) {
        scheme_len = 7u;
    } else if (len >= 8u && span_equal_ci(data, 8u, "https://")) {
        scheme_len = 8u;
        secure = true;
    } else {
        return H2_PAL_ERR_UNSUPPORTED;
    }

    size_t authority_end = scheme_len;
    while (authority_end < len && data[authority_end] != '/' &&
           data[authority_end] != '?') {
        if (data[authority_end] == '@' || data[authority_end] == ' ' ||
            data[authority_end] == '\t') {
            return H2_PAL_ERR_INVALID_ARG;
        }
        authority_end += 1u;
    }
    if (authority_end == scheme_len) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    const char *authority = data + scheme_len;
    size_t authority_len = authority_end - scheme_len;
    const char *host = authority;
    size_t host_len = authority_len;
    const char *port_data = NULL;
    size_t port_len = 0u;
    if (authority[0] == '[') {
        size_t closing = 1u;
        while (closing < authority_len && authority[closing] != ']') {
            closing += 1u;
        }
        if (closing == authority_len || closing == 1u) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        host = authority + 1u;
        host_len = closing - 1u;
        if (closing + 1u < authority_len) {
            if (authority[closing + 1u] != ':') {
                return H2_PAL_ERR_INVALID_ARG;
            }
            port_data = authority + closing + 2u;
            port_len = authority_len - closing - 2u;
        }
    } else {
        size_t colon = authority_len;
        for (size_t index = 0u; index < authority_len; ++index) {
            if (authority[index] == ':') {
                if (colon != authority_len) {
                    return H2_PAL_ERR_INVALID_ARG;
                }
                colon = index;
            }
        }
        if (colon != authority_len) {
            host_len = colon;
            port_data = authority + colon + 1u;
            port_len = authority_len - colon - 1u;
        }
    }
    if (host_len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    uint16_t port = secure ? 443u : 80u;
    if (port_data != NULL) {
        h2_pal_result_t rc = parse_port(port_data, port_len, &port);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }

    const h2_pal_mem_api_t *allocator = provider->config.allocator;
    out_url->host = duplicate_span(allocator, host, host_len);
    out_url->authority = duplicate_span(allocator, authority, authority_len);
    if (authority_end == len) {
        out_url->path = duplicate_span(allocator, "/", 1u);
        out_url->path_len = 1u;
    } else if (data[authority_end] == '?') {
        if (len - authority_end == SIZE_MAX) {
            h2_corehttp_url_deinit(provider, out_url);
            return H2_PAL_ERR_NO_SPACE;
        }
        out_url->path = (char *)h2_pal_mem_alloc(
            allocator, len - authority_end + 2u);
        if (out_url->path != NULL) {
            out_url->path[0] = '/';
            memcpy(out_url->path + 1u, data + authority_end,
                   len - authority_end);
            out_url->path[len - authority_end + 1u] = '\0';
            out_url->path_len = len - authority_end + 1u;
        }
    } else {
        out_url->path = duplicate_span(
            allocator, data + authority_end, len - authority_end);
        out_url->path_len = len - authority_end;
    }
    if (out_url->host == NULL || out_url->authority == NULL ||
        out_url->path == NULL) {
        h2_corehttp_url_deinit(provider, out_url);
        return H2_PAL_ERR_NO_MEMORY;
    }
    out_url->host_len = host_len;
    out_url->authority_len = authority_len;
    out_url->port = port;
    out_url->secure = secure;
    return H2_PAL_OK;
}

static h2_pal_result_t allocate_redirect(
    h2_corehttp_t *provider,
    const char *scheme,
    size_t scheme_len,
    const h2_corehttp_url_t *base,
    const char *path,
    size_t path_len,
    char **out_url,
    size_t *out_url_len) {
    if (scheme_len > SIZE_MAX - base->authority_len ||
        scheme_len + base->authority_len > SIZE_MAX - path_len) {
        return H2_PAL_ERR_NO_SPACE;
    }
    size_t len = scheme_len + base->authority_len + path_len;
    char *url = (char *)h2_pal_mem_alloc(provider->config.allocator, len + 1u);
    if (url == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(url, scheme, scheme_len);
    memcpy(url + scheme_len, base->authority, base->authority_len);
    memcpy(url + scheme_len + base->authority_len, path, path_len);
    url[len] = '\0';
    *out_url = url;
    *out_url_len = len;
    return H2_PAL_OK;
}

h2_pal_result_t h2_corehttp_resolve_redirect(
    h2_corehttp_t *provider,
    const h2_corehttp_url_t *base,
    const char *location,
    size_t location_len,
    char **out_url,
    size_t *out_url_len) {
    if (provider == NULL || base == NULL || location == NULL ||
        location_len == 0u || out_url == NULL || out_url_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_url = NULL;
    *out_url_len = 0u;
    if ((location_len >= 7u && span_equal_ci(location, 7u, "http://")) ||
        (location_len >= 8u && span_equal_ci(location, 8u, "https://"))) {
        *out_url = duplicate_span(provider->config.allocator, location,
                                  location_len);
        if (*out_url == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        *out_url_len = location_len;
        return H2_PAL_OK;
    }
    if (has_uri_scheme(location, location_len)) {
        return H2_PAL_ERR_UNSUPPORTED;
    }

    const char *scheme = base->secure ? "https://" : "http://";
    size_t scheme_len = base->secure ? 8u : 7u;
    if (location_len >= 2u && location[0] == '/' && location[1] == '/') {
        if (scheme_len - 2u > SIZE_MAX - location_len) {
            return H2_PAL_ERR_NO_SPACE;
        }
        size_t len = scheme_len - 2u + location_len;
        char *url = (char *)h2_pal_mem_alloc(
            provider->config.allocator, len + 1u);
        if (url == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        memcpy(url, scheme, scheme_len - 2u);
        memcpy(url + scheme_len - 2u, location, location_len);
        url[len] = '\0';
        *out_url = url;
        *out_url_len = len;
        return H2_PAL_OK;
    }
    if (location[0] == '/') {
        return allocate_redirect(provider, scheme, scheme_len, base, location,
                                 location_len, out_url, out_url_len);
    }

    size_t base_path_len = base->path_len;
    for (size_t index = 0u; index < base_path_len; ++index) {
        if (base->path[index] == '?') {
            base_path_len = index;
            break;
        }
    }
    if (location[0] == '?') {
        if (base_path_len == 0u) {
            base_path_len = 1u;
        }
        if (base_path_len > SIZE_MAX - location_len) {
            return H2_PAL_ERR_NO_SPACE;
        }
        size_t path_len = base_path_len + location_len;
        char *path = (char *)h2_pal_mem_alloc(
            provider->config.allocator, path_len);
        if (path == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        memcpy(path, base->path, base_path_len);
        memcpy(path + base_path_len, location, location_len);
        h2_pal_result_t rc = allocate_redirect(
            provider, scheme, scheme_len, base, path, path_len, out_url,
            out_url_len);
        h2_pal_mem_free(provider->config.allocator, path);
        return rc;
    }

    size_t directory_len = base_path_len;
    while (directory_len > 0u && base->path[directory_len - 1u] != '/') {
        directory_len -= 1u;
    }
    if (directory_len > SIZE_MAX - location_len) {
        return H2_PAL_ERR_NO_SPACE;
    }
    size_t path_len = directory_len + location_len;
    char *path = (char *)h2_pal_mem_alloc(
        provider->config.allocator, path_len);
    if (path == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(path, base->path, directory_len);
    memcpy(path + directory_len, location, location_len);
    h2_pal_result_t rc = allocate_redirect(
        provider, scheme, scheme_len, base, path, path_len, out_url,
        out_url_len);
    h2_pal_mem_free(provider->config.allocator, path);
    return rc;
}
