#ifndef H2_RUNTIME_TASK_NAMES_H
#define H2_RUNTIME_TASK_NAMES_H

#define H2_RUNTIME_INPUT_TASK_NAME_VALUE "$runtime/input"
#define H2_RUNTIME_NFC_TASK_NAME_VALUE "$runtime/nfc"

#ifdef __cplusplus
extern "C" {
#endif

extern const char
    h2_runtime_input_task_name[sizeof(H2_RUNTIME_INPUT_TASK_NAME_VALUE)];
extern const char
    h2_runtime_nfc_task_name[sizeof(H2_RUNTIME_NFC_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
