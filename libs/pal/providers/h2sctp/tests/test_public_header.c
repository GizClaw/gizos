#include "h2_sctp.h"

int main(void) {
    h2_sctp_t *provider = NULL;
    const h2_sctp_config_t config = {0};
    (void)h2_sctp_create(&config, &provider);
    (void)h2_sctp_api(provider);
    (void)h2_sctp_destroy(&provider);
    return 0;
}
