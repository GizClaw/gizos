#include "h2_loader_stage.h"

#include <string.h>

int h2_loader_stage_writer_begin(
    h2_loader_stage_writer_t *writer, h2_loader_package_t *package,
    const h2_pal_pref_api_t *pref, uint64_t size, const char *checksum,
    const char *board, const char *target) {
    if (writer == NULL || package == NULL || pref == NULL || size == 0u ||
        checksum == NULL || strlen(checksum) != 64u ||
        strspn(checksum, "0123456789abcdef") != 64u ||
        board == NULL || target == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (writer->file != NULL || writer->started) return H2_PAL_ERR_BUSY;
    memset(writer, 0, sizeof(*writer));
    writer->package = package;
    writer->pref = pref;
    writer->expected = size;
    writer->board = board;
    writer->target = target;
    memcpy(writer->checksum, checksum, 65u);
    int rc = h2_loader_stage_begin(pref);
    if (rc != H2_PAL_OK) return rc;
    writer->started = 1;
    return h2_pal_fs_open(package->config.fs, package->config.package_path,
                         H2_PAL_FS_OPEN_WRITE_TRUNCATE, &writer->file);
}

int h2_loader_stage_writer_write(
    h2_loader_stage_writer_t *writer, const uint8_t *data, size_t size,
    unsigned *out_percent) {
    if (writer == NULL || writer->file == NULL || out_percent == NULL)
        return H2_PAL_ERR_INVALID_STATE;
    if ((data == NULL && size != 0u) || writer->written > writer->expected ||
        size > writer->expected - writer->written) return H2_PAL_ERR_FORMAT;
    size_t written = 0;
    int rc = h2_pal_fs_write(writer->package->config.fs, writer->file,
                            data, size, &written);
    if (rc == H2_PAL_OK && written != size) rc = H2_PAL_ERR_IO;
    if (rc != H2_PAL_OK) return rc;
    writer->written += written;
    while (writer->percent < 100u) {
        unsigned next = writer->percent + 1u;
        uint64_t threshold = (writer->expected / 100u) * next +
            ((writer->expected % 100u) * next + 99u) / 100u;
        if (writer->written < threshold) break;
        writer->percent = next;
    }
    *out_percent = writer->percent;
    return H2_PAL_OK;
}

int h2_loader_stage_writer_inspect(
    h2_loader_stage_writer_t *writer,
    const h2_loader_package_inspection_t **out_inspection) {
    if (writer == NULL || writer->file == NULL || out_inspection == NULL)
        return H2_PAL_ERR_INVALID_STATE;
    *out_inspection = NULL;
    h2_loader_package_t *package = writer->package;
    int rc = h2_pal_fs_sync(package->config.fs, writer->file);
    int closed = h2_pal_fs_close(package->config.fs, writer->file);
    writer->file = NULL;
    if (rc == H2_PAL_OK) rc = closed;
    if (rc == H2_PAL_OK && writer->written != writer->expected)
        rc = H2_PAL_ERR_FORMAT;
    if (rc == H2_PAL_OK)
        rc = h2_loader_package_verify_path(package, package->config.package_path,
                                           writer->expected, writer->checksum);
    if (rc == H2_PAL_OK)
        rc = h2_loader_package_inspect_path(package, package->config.package_path,
                                            &writer->inspection);
    const h2_loader_package_inspection_t *inspection = &writer->inspection;
    if (rc == H2_PAL_OK &&
        (inspection->legacy || inspection->manifest.role != H2_LOADER_IMAGE_ROLE_APP ||
         strcmp(inspection->manifest.board, writer->board) ||
         strcmp(inspection->manifest.target, writer->target) ||
         (inspection->data_checksum_len != 64u &&
          !(inspection->data_checksum_len == 65u && inspection->data_checksum[64] == '\n'))))
        rc = H2_PAL_ERR_FORMAT;
    if (rc == H2_PAL_OK) {
        writer->verified = 1;
        *out_inspection = inspection;
    }
    return rc;
}

int h2_loader_stage_writer_commit(h2_loader_stage_writer_t *writer) {
    if (writer == NULL || !writer->started || !writer->verified)
        return H2_PAL_ERR_INVALID_STATE;
    return h2_loader_stage_commit_inspection(writer->pref, writer->expected,
                                            writer->checksum, &writer->inspection, NULL);
}

int h2_loader_stage_writer_abort(h2_loader_stage_writer_t *writer) {
    if (writer == NULL) return H2_PAL_ERR_INVALID_ARG;
    int rc = H2_PAL_OK;
    if (writer->file != NULL) {
        rc = h2_pal_fs_close(writer->package->config.fs, writer->file);
        writer->file = NULL;
    }
    if (writer->started) {
        int aborted = h2_loader_stage_abort(writer->package->config.fs,
                                            writer->pref, writer->package->config.package_path);
        if (rc == H2_PAL_OK) rc = aborted;
    }
    memset(writer, 0, sizeof(*writer));
    return rc;
}
