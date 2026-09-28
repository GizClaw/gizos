#ifndef H2_ANDROID_TLS_INTERNAL_H
#define H2_ANDROID_TLS_INTERNAL_H
/* Crypto and each HTTP owner share one full WolfSSL provider and OS entropy. */
int h2_android_tls_acquire(void);
int h2_android_tls_release(void);
#endif
