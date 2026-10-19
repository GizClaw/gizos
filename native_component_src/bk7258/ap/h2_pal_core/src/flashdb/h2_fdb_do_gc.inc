/* Stop on failed relocation; an incomplete copy cannot justify source erase. */
static bool do_gc(kv_sec_info_t sector, void *arg1, void *arg2)
{
    struct fdb_kv kv;
    fdb_kvdb_t db = arg1;
    (void)arg2;
    if (sector->check_ok && (sector->status.dirty == FDB_SECTOR_DIRTY_TRUE ||
            sector->status.dirty == FDB_SECTOR_DIRTY_GC)) {
        uint8_t status_table[FDB_DIRTY_STATUS_TABLE_SIZE];
        if (_fdb_write_status((fdb_db_t)db, sector->addr + SECTOR_DIRTY_OFFSET,
                status_table, FDB_SECTOR_DIRTY_STATUS_NUM, FDB_SECTOR_DIRTY_GC, true) != FDB_NO_ERR)
            return true;
        kv.addr.start = sector->addr + SECTOR_HDR_DATA_SIZE;
        do {
            read_kv(db, &kv);
            if (h2_bk_pref_flash_faulted()) return true;
            if (kv.crc_is_ok && (kv.status == FDB_KV_WRITE || kv.status == FDB_KV_PRE_DELETE)) {
                fdb_err_t move_result = move_kv(db, &kv);
                if (move_result != FDB_NO_ERR) {
                    /* Leave this live source intact. Continue past a space
                     * shortage so later garbage-only sectors can be erased;
                     * stopping at the first live sector deadlocks recovery. */
                    return move_result != FDB_SAVED_FULL;
                }
            }
        } while ((kv.addr.start = get_next_kv_addr(db, sector, &kv)) != FAILED_ADDR);
        if (format_sector(db, sector->addr, SECTOR_NOT_COMBINED) != FDB_NO_ERR) return true;
    }
    return false;
}
