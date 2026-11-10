#ifndef H2_ANDROID_HTTP_TEST_JNI_H
#define H2_ANDROID_HTTP_TEST_JNI_H
/* Host-only opaque handles needed to compile the real platform declaration.
 * HTTP and Crypto do not invoke JNI; Android builds use their native headers.
 */
typedef const struct JNINativeInterface_* JNIEnv;
typedef struct _jobject* jobject;
#endif
