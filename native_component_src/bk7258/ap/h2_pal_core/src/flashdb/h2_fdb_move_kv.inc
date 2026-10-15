/* Same on-flash FlashDB encoding. A failed copy must retain its source. */
static fdb_err_t move_kv(fdb_kvdb_t db, fdb_kv_t kv)
{
    fdb_err_t result;
    uint8_t status_table[KV_STATUS_TABLE_SIZE];
    uint32_t kv_addr;
    struct kvdb_sec_info sector;

    if (kv->status == FDB_KV_WRITE) {
        result = del_kv(db, NULL, kv, false);
        if (result != FDB_NO_ERR) return result;
    }
    kv_addr = alloc_kv(db, &sector, kv->len);
    if (kv_addr == FAILED_ADDR) return FDB_SAVED_FULL;
    if (db->in_recovery_check) {
        struct fdb_kv kv_bak;
        char name[FDB_KV_NAME_MAX + 1] = {0};
        strncpy(name, kv->name, kv->name_len);
        if (find_kv_no_cache(db, name, &kv_bak)) {
            /* A complete duplicate already exists, so retiring this source is safe. */
            return del_kv(db, NULL, kv, true);
        }
    }
    result = update_sec_status(db, &sector, kv->len, NULL);
    if (result != FDB_NO_ERR) return result;
    result = _fdb_write_status((fdb_db_t)db, kv_addr, status_table,
        FDB_KV_STATUS_NUM, FDB_KV_PRE_WRITE, false);
    if (result != FDB_NO_ERR) return result;
    uint8_t buf[32];
    size_t kv_len = kv->len - KV_MAGIC_OFFSET;
    for (size_t offset = 0; offset < kv_len;) {
        size_t size = kv_len - offset;
        if (size > sizeof(buf)) size = sizeof(buf);
        result = _fdb_flash_read((fdb_db_t)db,
            kv->addr.start + KV_MAGIC_OFFSET + offset, buf, FDB_WG_ALIGN(size));
        if (result != FDB_NO_ERR) return result;
        result = _fdb_flash_write((fdb_db_t)db,
            kv_addr + KV_MAGIC_OFFSET + offset, buf, size, true);
        if (result != FDB_NO_ERR) return result;
        offset += size;
    }
    result = _fdb_write_status((fdb_db_t)db, kv_addr, status_table,
        FDB_KV_STATUS_NUM, FDB_KV_WRITE, true);
    if (result != FDB_NO_ERR) return result;
#ifdef FDB_KV_USING_CACHE
    update_sector_cache(db, FDB_ALIGN_DOWN(kv_addr, db_sec_size(db)),
        kv_addr + KV_HDR_DATA_SIZE + FDB_WG_ALIGN(kv->name_len) + FDB_WG_ALIGN(kv->value_len));
    update_kv_cache(db, kv->name, kv->name_len, kv_addr);
#endif
    return del_kv(db, NULL, kv, true);
}
