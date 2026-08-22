#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PSN_ACCOUNT_ID_SIZE 8

/*
 * Decode a PSN account ID from chiaki-ng's canonical base64 representation or
 * from the unsigned decimal representation used by some older/manual exports.
 * The byte representation is always the eight-byte little-endian form expected
 * by libchiaki.
 */
bool psn_account_id_decode(
    const char *value, uint8_t out[PSN_ACCOUNT_ID_SIZE]);

/* Decode to the numeric credential used by discovery wakeup packets. */
bool psn_account_id_to_uint64(const char *value, uint64_t *out);

/* Decode and format the decimal account ID expected by the PSN web API. */
bool psn_account_id_to_decimal(
    const char *value, char *out, size_t out_size);
