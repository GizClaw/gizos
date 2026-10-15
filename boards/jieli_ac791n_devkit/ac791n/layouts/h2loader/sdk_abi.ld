/* Require the public non-blocking SDK primitives in the actual native link.
 * EXTERN prevents LTO/GC from making the ABI check vacuously pass. */
EXTERN(os_mutex_accept os_sem_accept)
ASSERT(DEFINED(os_mutex_accept), "WL82 SDK missing os_mutex_accept")
ASSERT(DEFINED(os_sem_accept), "WL82 SDK missing os_sem_accept")
