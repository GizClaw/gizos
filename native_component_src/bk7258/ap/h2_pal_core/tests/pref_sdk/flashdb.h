#ifndef H2_TEST_FLASHDB_H
#define H2_TEST_FLASHDB_H
#include <stdbool.h>
#include <stddef.h>
#define H2_BK_PREF_FLASHDB_PATH "test"
#define FDB_KV_NAME_MAX 64
#define FDB_KVDB_CTRL_SET_LOCK 1
#define FDB_KVDB_CTRL_SET_UNLOCK 2
typedef enum {
  FDB_NO_ERR,
  FDB_KV_NAME_ERR,
  FDB_KV_NAME_EXIST,
  FDB_SAVED_FULL,
  FDB_INIT_FAILED,
  FDB_PART_NOT_FOUND,
  FDB_ERASE_ERR,
  FDB_READ_ERR,
  FDB_WRITE_ERR
} fdb_err_t;
struct fdb_db {
  void *user_data;
};
typedef struct fdb_db *fdb_db_t;
struct fdb_kvdb {
  struct fdb_db parent;
};
typedef struct fdb_kvdb *fdb_kvdb_t;
struct fdb_blob {
  void *buf;
  size_t size;
};
typedef struct fdb_blob *fdb_blob_t;
struct fdb_kv {
  char name[FDB_KV_NAME_MAX];
  size_t value_len;
};
struct fdb_kv_iterator {
  size_t index;
  struct fdb_kv curr_kv;
};
void fdb_kvdb_control(fdb_kvdb_t db, int command, void *value);
fdb_err_t fdb_kvdb_init(fdb_kvdb_t db, const char *name, const char *path,
                        void *defaults, void *user);
fdb_blob_t fdb_blob_make(fdb_blob_t blob, const void *data, size_t length);
struct fdb_kv *fdb_kv_get_obj(fdb_kvdb_t db, const char *key,
                              struct fdb_kv *out);
size_t fdb_kv_get_blob(fdb_kvdb_t db, const char *key, fdb_blob_t blob);
fdb_err_t fdb_kv_set_blob(fdb_kvdb_t db, const char *key, fdb_blob_t blob);
fdb_err_t fdb_kv_del(fdb_kvdb_t db, const char *key);
void fdb_kv_iterator_init(struct fdb_kv_iterator *iterator);
bool fdb_kv_iterate(fdb_kvdb_t db, struct fdb_kv_iterator *iterator);
#endif
