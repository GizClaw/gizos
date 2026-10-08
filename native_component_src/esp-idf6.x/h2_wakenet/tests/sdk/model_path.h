#ifndef FAKE_MODEL_PATH_H
#define FAKE_MODEL_PATH_H
typedef struct srmodel_list { int num; } srmodel_list_t;
srmodel_list_t *get_static_srmodels(void);
srmodel_list_t *srmodel_load(const void *root);
int esp_srmodel_exists(srmodel_list_t *models, char *name);
void srmodel_host_deinit(srmodel_list_t *models);
#endif
