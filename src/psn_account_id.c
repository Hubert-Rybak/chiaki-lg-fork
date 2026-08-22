#include "psn_account_id.h"

#include <chiaki/base64.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_unsigned_decimal(const char *value)
{
    if (!value || !value[0])
        return false;

    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
    }
    return true;
}

static bool base64_input_is_ascii(const char *value)
{
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p > 0x7f)
            return false;
    }
    return true;
}

bool psn_account_id_decode(
    const char *value, uint8_t out[PSN_ACCOUNT_ID_SIZE])
{
    if (!value || !out || !value[0])
        return false;

    uint8_t decoded[PSN_ACCOUNT_ID_SIZE] = {0};
    if (is_unsigned_decimal(value)) {
        errno = 0;
        char *end = NULL;
        unsigned long long parsed = strtoull(value, &end, 10);
        if (errno == ERANGE || !end || *end != '\0' ||
            parsed > (unsigned long long)UINT64_MAX)
            return false;

        uint64_t id = (uint64_t)parsed;
        for (size_t i = 0; i < PSN_ACCOUNT_ID_SIZE; ++i) {
            decoded[i] = (uint8_t)(id & 0xffu);
            id >>= 8;
        }
    } else {
        if (!base64_input_is_ascii(value))
            return false;

        size_t decoded_size = sizeof(decoded);
        if (chiaki_base64_decode(value, strlen(value), decoded, &decoded_size) !=
                CHIAKI_ERR_SUCCESS ||
            decoded_size != sizeof(decoded))
            return false;
    }

    memcpy(out, decoded, sizeof(decoded));
    return true;
}

bool psn_account_id_to_uint64(const char *value, uint64_t *out)
{
    if (!out)
        return false;

    uint8_t decoded[PSN_ACCOUNT_ID_SIZE];
    if (!psn_account_id_decode(value, decoded))
        return false;

    uint64_t id = 0;
    for (size_t i = 0; i < PSN_ACCOUNT_ID_SIZE; ++i)
        id |= (uint64_t)decoded[i] << (i * 8u);
    *out = id;
    return true;
}

bool psn_account_id_to_decimal(
    const char *value, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return false;

    uint64_t id = 0;
    if (!psn_account_id_to_uint64(value, &id))
        return false;

    int written = snprintf(out, out_size, "%" PRIu64, id);
    return written >= 0 && (size_t)written < out_size;
}
