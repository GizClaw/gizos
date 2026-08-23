#include "h2_sctp.h"

int main() {
    h2_sctp_t *provider = nullptr;
    const h2_sctp_config_t config{};
    (void)h2_sctp_create(&config, &provider);
    (void)h2_sctp_api(provider);
    (void)h2_sctp_destroy(&provider);
    return 0;
}
