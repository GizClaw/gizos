/*
 * Portable BK3633 BinConvert compatibility implementation.
 *
 * This implements the -oad operation used by h2vivi/firmwares. It is based on
 * the on-flash format consumed by the BK3633 BIM and differential tests against
 * vendor-generated images. It intentionally does not implement unrelated
 * BinConvert modes or non-zero image encryption keys.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OAD_HEADER_SIZE 16U
#define CRC_LINE_DATA_SIZE 32U
#define CRC_LINE_SIZE 34U
#define STACK_UID UINT32_C(0x53535353)
#define APP_UID UINT32_C(0x42424242)

struct options {
    const char *boot_path;
    const char *stack_path;
    const char *app_path;
    uint32_t stack_addr;
    uint32_t app_addr;
    uint16_t version;
    uint16_t rom_version;
    int have_stack_addr;
    int have_app_addr;
    int have_version;
    int have_rom_version;
    int have_keys;
};

struct buffer {
    uint8_t *data;
    size_t size;
};

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s -oad BOOT.bin STACK.bin APP.bin "
            "-m STACK_ADDR -l APP_ADDR -v VERSION -rom_v ROM_VERSION "
            "-e 00000000 00000000 00000000 00000000\n",
            program);
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return -1;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int is_zero_key(const char *text)
{
    size_t i;

    if (strlen(text) != 8U) {
        return 0;
    }
    for (i = 0; i < 8U; ++i) {
        if (text[i] != '0') {
            return 0;
        }
    }
    return 1;
}

static int parse_options(int argc, char **argv, struct options *options)
{
    int i;

    memset(options, 0, sizeof(*options));
    if (argc < 5 || strcmp(argv[1], "-oad") != 0) {
        return -1;
    }
    options->boot_path = argv[2];
    options->stack_path = argv[3];
    options->app_path = argv[4];

    for (i = 5; i < argc;) {
        uint32_t value;

        if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "-l") == 0 ||
             strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "-rom_v") == 0) &&
            i + 1 < argc) {
            if (parse_u32(argv[i + 1], &value) != 0) {
                fprintf(stderr, "invalid value for %s: %s\n", argv[i], argv[i + 1]);
                return -1;
            }
            if (strcmp(argv[i], "-m") == 0) {
                options->stack_addr = value;
                options->have_stack_addr = 1;
            } else if (strcmp(argv[i], "-l") == 0) {
                options->app_addr = value;
                options->have_app_addr = 1;
            } else if (strcmp(argv[i], "-v") == 0) {
                if (value > UINT16_MAX) {
                    fprintf(stderr, "version does not fit in 16 bits: %s\n", argv[i + 1]);
                    return -1;
                }
                options->version = (uint16_t)value;
                options->have_version = 1;
            } else {
                if (value > UINT16_MAX) {
                    fprintf(stderr, "ROM version does not fit in 16 bits: %s\n", argv[i + 1]);
                    return -1;
                }
                options->rom_version = (uint16_t)value;
                options->have_rom_version = 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "-e") == 0 && i + 4 < argc) {
            int key;

            for (key = 1; key <= 4; ++key) {
                if (!is_zero_key(argv[i + key])) {
                    fprintf(stderr,
                            "non-zero encryption keys are not supported by this compatibility tool\n");
                    return -1;
                }
            }
            options->have_keys = 1;
            i += 5;
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
            return -1;
        }
    }

    if (!options->have_stack_addr || !options->have_app_addr ||
        !options->have_version || !options->have_rom_version || !options->have_keys) {
        fprintf(stderr, "missing required -oad option\n");
        return -1;
    }
    if ((options->stack_addr % CRC_LINE_DATA_SIZE) != 0U ||
        (options->app_addr % CRC_LINE_DATA_SIZE) != 0U) {
        fprintf(stderr, "stack and app addresses must be 32-byte aligned\n");
        return -1;
    }
    if (options->stack_addr >= options->app_addr) {
        fprintf(stderr, "stack address must precede app address\n");
        return -1;
    }
    return 0;
}

static int read_file(const char *path, struct buffer *buffer)
{
    FILE *file;
    long length;

    memset(buffer, 0, sizeof(*buffer));
    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot determine size of %s\n", path);
        fclose(file);
        return -1;
    }
    buffer->size = (size_t)length;
    buffer->data = malloc(buffer->size == 0U ? 1U : buffer->size);
    if (buffer->data == NULL) {
        fprintf(stderr, "out of memory reading %s\n", path);
        fclose(file);
        return -1;
    }
    if (buffer->size != 0U && fread(buffer->data, 1, buffer->size, file) != buffer->size) {
        fprintf(stderr, "cannot read %s\n", path);
        free(buffer->data);
        memset(buffer, 0, sizeof(*buffer));
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (size != 0U && fwrite(data, 1, size, file) != size) {
        fprintf(stderr, "cannot write %s\n", path);
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "cannot close %s\n", path);
        return -1;
    }
    return 0;
}

static uint16_t crc16_line(const uint8_t data[CRC_LINE_DATA_SIZE])
{
    uint16_t crc = UINT16_C(0xffff);
    size_t byte_index;

    for (byte_index = 0; byte_index < CRC_LINE_DATA_SIZE; ++byte_index) {
        int bit_index;
        for (bit_index = 7; bit_index >= 0; --bit_index) {
            uint16_t data_bit = (uint16_t)((data[byte_index] >> bit_index) & 1U);
            uint16_t top_bit = (uint16_t)((crc >> 15) & 1U);
            crc = (uint16_t)((uint16_t)(crc << 1) | data_bit);
            crc ^= (uint16_t)((data_bit << 15) | (data_bit << 2));
            crc ^= (uint16_t)((top_bit << 15) | (top_bit << 2) | top_bit);
        }
    }
    return crc;
}

static uint32_t crc32_image(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;

    for (i = 0; i < size; ++i) {
        int bit;
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) != 0U ? UINT32_C(0xedb88320) : 0U);
        }
    }
    return crc;
}

static size_t flash_address(uint32_t code_address)
{
    return ((size_t)code_address * CRC_LINE_SIZE) / CRC_LINE_DATA_SIZE;
}

static void put_u16_le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void put_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static int oad_length_words(size_t package_size, uint16_t *length_words)
{
    if ((package_size % 4U) != 0U || package_size / 4U > UINT16_MAX) {
        return -1;
    }
    if (length_words != NULL) {
        *length_words = (uint16_t)(package_size / 4U);
    }
    return 0;
}

static int put_oad_header(uint8_t *image, size_t image_size, size_t offset,
                          uint16_t version, uint16_t rom_version, uint32_t uid,
                          const char *package_name)
{
    size_t package_size;
    uint16_t length_words;

    if (offset > image_size || image_size - offset < OAD_HEADER_SIZE) {
        return -1;
    }
    package_size = image_size - offset;
    if (oad_length_words(package_size, &length_words) != 0) {
        fprintf(stderr,
                "%s OAD package size cannot be represented in its 16-bit header\n",
                package_name);
        return -1;
    }
    put_u32_le(image + offset,
               crc32_image(image + offset + OAD_HEADER_SIZE,
                           package_size - OAD_HEADER_SIZE));
    put_u16_le(image + offset + 4U, version);
    put_u16_le(image + offset + 6U, length_words);
    put_u32_le(image + offset + 8U, uid);
    image[offset + 12U] = UINT8_C(0xff);
    image[offset + 13U] = UINT8_C(0xff);
    put_u16_le(image + offset + 14U, rom_version);
    return 0;
}

static char *output_name(const char *app_path, const char *suffix)
{
    const char *slash = strrchr(app_path, '/');
    const char *dot = strrchr(app_path, '.');
    size_t stem_size;
    char *name;

    if (dot == NULL || (slash != NULL && dot < slash)) {
        dot = app_path + strlen(app_path);
    }
    stem_size = (size_t)(dot - app_path);
    name = malloc(stem_size + strlen(suffix) + 1U);
    if (name == NULL) {
        return NULL;
    }
    memcpy(name, app_path, stem_size);
    strcpy(name + stem_size, suffix);
    return name;
}

static int build_image(const struct options *options,
                       const struct buffer *boot, const struct buffer *stack,
                       const struct buffer *app, struct buffer *image,
                       size_t *stack_header_offset, size_t *app_header_offset,
                       int *has_stack_oad)
{
    uint8_t *logical = NULL;
    size_t logical_size;
    size_t padded_logical_size;
    size_t line_count;
    size_t base_image_size;
    size_t pad_size;
    size_t line;

    if (boot->size > options->stack_addr ||
        stack->size > (size_t)options->app_addr - options->stack_addr ||
        app->size > SIZE_MAX - options->app_addr) {
        fprintf(stderr, "input images overlap or are too large\n");
        return -1;
    }
    logical_size = (size_t)options->app_addr + app->size;
    if (logical_size > SIZE_MAX - (CRC_LINE_DATA_SIZE - 1U)) {
        fprintf(stderr, "input image is too large\n");
        return -1;
    }
    padded_logical_size = (logical_size + CRC_LINE_DATA_SIZE - 1U) &
                          ~(CRC_LINE_DATA_SIZE - 1U);
    logical = malloc(padded_logical_size == 0U ? 1U : padded_logical_size);
    if (logical == NULL) {
        fprintf(stderr, "out of memory building image\n");
        return -1;
    }
    memset(logical, 0xff, padded_logical_size);
    memcpy(logical, boot->data, boot->size);
    memcpy(logical + options->stack_addr, stack->data, stack->size);
    memcpy(logical + options->app_addr, app->data, app->size);

    line_count = padded_logical_size / CRC_LINE_DATA_SIZE;
    if (line_count > SIZE_MAX / CRC_LINE_SIZE) {
        free(logical);
        return -1;
    }
    base_image_size = line_count * CRC_LINE_SIZE;
    *stack_header_offset = flash_address(options->stack_addr) - OAD_HEADER_SIZE;
    *app_header_offset = flash_address(options->app_addr) - OAD_HEADER_SIZE;
    if (*stack_header_offset > base_image_size || *app_header_offset > base_image_size) {
        free(logical);
        return -1;
    }

    /* Vendor BinConvert always advances to the next 256-byte stack package boundary. */
    pad_size = 256U - ((base_image_size - *stack_header_offset) & 0xffU);
    if (base_image_size > SIZE_MAX - pad_size) {
        free(logical);
        return -1;
    }
    image->size = base_image_size + pad_size;
    image->data = malloc(image->size);
    if (image->data == NULL) {
        fprintf(stderr, "out of memory building image\n");
        free(logical);
        return -1;
    }

    for (line = 0; line < line_count; ++line) {
        const uint8_t *source = logical + line * CRC_LINE_DATA_SIZE;
        uint8_t *destination = image->data + line * CRC_LINE_SIZE;
        uint16_t crc = crc16_line(source);

        memcpy(destination, source, CRC_LINE_DATA_SIZE);
        destination[CRC_LINE_DATA_SIZE] = (uint8_t)(crc >> 8);
        destination[CRC_LINE_DATA_SIZE + 1U] = (uint8_t)crc;
    }
    memset(image->data + base_image_size, 0xff, pad_size);
    free(logical);

    /* The BIM metadata line is deliberately emitted without a line CRC. */
    if (image->size >= 0x132U) {
        image->data[0x130U] = 0;
        image->data[0x131U] = 0;
    }

    if (put_oad_header(image->data, image->size, *app_header_offset,
                       options->version, options->rom_version, APP_UID,
                       "application") != 0) {
        free(image->data);
        memset(image, 0, sizeof(*image));
        return -1;
    }

    if (oad_length_words(image->size - *stack_header_offset, NULL) != 0) {
        /*
         * The vendor tool silently truncates this length to 16 bits, which
         * produces an OAD package that the BIM cannot validate or copy.  The
         * merged factory image and the application-only OAD remain valid, but
         * an all-image OAD cannot be represented by the BK3633 protocol.
         */
        memset(image->data + *stack_header_offset, 0xff, OAD_HEADER_SIZE);
        *has_stack_oad = 0;
    } else {
        if (put_oad_header(image->data, image->size, *stack_header_offset,
                           options->version, options->rom_version, STACK_UID,
                           "combined stack and application") != 0) {
            free(image->data);
            memset(image, 0, sizeof(*image));
            return -1;
        }
        *has_stack_oad = 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct options options;
    struct buffer boot = {0};
    struct buffer stack = {0};
    struct buffer app = {0};
    struct buffer image = {0};
    size_t stack_header_offset = 0;
    size_t app_header_offset = 0;
    int has_stack_oad = 0;
    char *merge_name = NULL;
    char *oad_name = NULL;
    char *stack_oad_name = NULL;
    int result = EXIT_FAILURE;

    if (parse_options(argc, argv, &options) != 0) {
        usage(stderr, argv[0]);
        goto cleanup;
    }
    if (read_file(options.boot_path, &boot) != 0 ||
        read_file(options.stack_path, &stack) != 0 ||
        read_file(options.app_path, &app) != 0) {
        goto cleanup;
    }
    if (build_image(&options, &boot, &stack, &app, &image,
                    &stack_header_offset, &app_header_offset,
                    &has_stack_oad) != 0) {
        goto cleanup;
    }

    merge_name = output_name(options.app_path, "_merge_crc.bin");
    oad_name = output_name(options.app_path, "_oad.bin");
    stack_oad_name = output_name(options.app_path, "_stack_oad.bin");
    if (merge_name == NULL || oad_name == NULL || stack_oad_name == NULL) {
        fprintf(stderr, "out of memory creating output names\n");
        goto cleanup;
    }
    if (!has_stack_oad && remove(stack_oad_name) != 0 && errno != ENOENT) {
        fprintf(stderr, "cannot remove stale %s: %s\n",
                stack_oad_name, strerror(errno));
        goto cleanup;
    }
    if (write_file(merge_name, image.data, image.size) != 0 ||
        write_file(oad_name, image.data + app_header_offset,
                   image.size - app_header_offset) != 0 ||
        (has_stack_oad &&
         write_file(stack_oad_name, image.data + stack_header_offset,
                    image.size - stack_header_offset) != 0)) {
        goto cleanup;
    }

    printf("generated %s (%zu bytes)\n", merge_name, image.size);
    if (!has_stack_oad) {
        printf("omitted %s: combined stack and application OAD exceeds "
               "the 16-bit length limit\n",
               stack_oad_name);
    }
    result = EXIT_SUCCESS;

cleanup:
    free(boot.data);
    free(stack.data);
    free(app.data);
    free(image.data);
    free(merge_name);
    free(oad_name);
    free(stack_oad_name);
    return result;
}
