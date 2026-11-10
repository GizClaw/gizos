#ifndef H2_IOS_TLS_INTERNAL_H
#define H2_IOS_TLS_INTERNAL_H
/* Crypto, HTTP and WebRTC owners share one full WolfSSL provider and OS
 * entropy. */
int h2_ios_tls_acquire(void);
int h2_ios_tls_release(void);
/* Transfer this owner to backend cleanup if timeout work is still active. */
void h2_ios_tls_release_deferred(void);
#endif
