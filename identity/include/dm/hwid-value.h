/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef DM_HWID_VALUE_H
#define DM_HWID_VALUE_H
#define DM_HWID_TEXT_SIZE 17U

static inline int dm_hwid_value_valid(const unsigned char value[8])
{
    unsigned int product = ((value[0] & 15U) << 4) | (value[1] >> 4);
    unsigned int project = ((value[1] & 15U) << 12) | (value[2] << 4) | (value[3] >> 4);
    unsigned int model = ((value[3] & 15U) << 12) | (value[4] << 4) | (value[5] >> 4);
    return value[0] >> 4 == 1 && product && project && model &&
           !(value[6] & 15U) && !value[7];
}

static inline int dm_hwid_parse(const char *text, unsigned char value[8])
{
    for (unsigned int i = 0; i < 16; i++) {
        unsigned char c = (unsigned char)text[i];
        unsigned int n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        else return 0;
        if (!(i & 1)) value[i / 2] = (unsigned char)(n << 4);
        else value[i / 2] |= (unsigned char)n;
    }
    return !text[16] && dm_hwid_value_valid(value);
}

static inline void dm_hwid_format(const unsigned char value[8], char text[17])
{
    const char *hex = "0123456789abcdef";
    for (unsigned int i = 0; i < 8; i++) {
        text[2 * i] = hex[value[i] >> 4]; text[2 * i + 1] = hex[value[i] & 15];
    }
    text[16] = 0;
}

#endif
