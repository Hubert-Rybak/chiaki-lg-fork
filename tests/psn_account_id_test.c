#include "psn_account_id.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void assert_bytes(const uint8_t *actual, const uint8_t *expected)
{
    assert(memcmp(actual, expected, PSN_ACCOUNT_ID_SIZE) == 0);
}

int main(void)
{
    static const uint8_t zero[PSN_ACCOUNT_ID_SIZE] = {0};
    static const uint8_t sequence[PSN_ACCOUNT_ID_SIZE] = {
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    };
    static const uint8_t maximum[PSN_ACCOUNT_ID_SIZE] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };

    uint8_t decoded[PSN_ACCOUNT_ID_SIZE];
    uint64_t numeric = 0;
    char decimal[32];

    assert(psn_account_id_decode("AAAAAAAAAAA=", decoded));
    assert_bytes(decoded, zero);
    assert(psn_account_id_decode("72623859790382856", decoded));
    assert_bytes(decoded, sequence);
    assert(psn_account_id_decode("CAcGBQQDAgE=", decoded));
    assert_bytes(decoded, sequence);
    assert(psn_account_id_decode("18446744073709551615", decoded));
    assert_bytes(decoded, maximum);

    assert(psn_account_id_to_uint64("CAcGBQQDAgE=", &numeric));
    assert(numeric == UINT64_C(72623859790382856));
    assert(psn_account_id_to_decimal("CAcGBQQDAgE=", decimal, sizeof(decimal)));
    assert(strcmp(decimal, "72623859790382856") == 0);

    assert(!psn_account_id_decode("", decoded));
    assert(!psn_account_id_decode("not-base64", decoded));
    assert(!psn_account_id_decode("AAAA", decoded));
    assert(!psn_account_id_decode("AAAAAAAAAAAAAAAA", decoded));
    assert(!psn_account_id_decode("18446744073709551616", decoded));
    assert(!psn_account_id_decode("-1", decoded));
    assert(!psn_account_id_to_decimal("AAAAAAAAAAA=", decimal, 1));

    puts("PSN account ID compatibility tests passed");
    return 0;
}
