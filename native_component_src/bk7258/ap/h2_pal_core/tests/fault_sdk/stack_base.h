#ifndef H2_TEST_FAULT_STACK_BASE_H
#define H2_TEST_FAULT_STACK_BASE_H
/* SDK interface shim. The SDK-backed probe instead includes the sealed
 * SDK's real header, and the firmware build validates its native ABI. */
typedef struct {
  long r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12;
  long sp, lr, pc, xpsr, msp, psp, primask, basepri, faultmask, control, fpscr;
} SAVED_CONTEXT;
#endif
