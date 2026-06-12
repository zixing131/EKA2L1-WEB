/*
 * Copyright (c) 2019 EKA2L1 Team
 * 
 * This file is part of EKA2L1 project.
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <common/crypt.h>
#include <string>

namespace eka2l1::crypt {
    static const std::uint32_t crc_tab[256] = {
        0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7, 0x8108, 0x9129, 0xa14a,
        0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef, 0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294,
        0x72f7, 0x62d6, 0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de, 0x2462,
        0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485, 0xa56a, 0xb54b, 0x8528, 0x9509,
        0xe5ee, 0xf5cf, 0xc5ac, 0xd58d, 0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695,
        0x46b4, 0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc, 0x48c4, 0x58e5,
        0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823, 0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948,
        0x9969, 0xa90a, 0xb92b, 0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
        0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a, 0x6ca6, 0x7c87, 0x4ce4,
        0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41, 0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b,
        0x8d68, 0x9d49, 0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70, 0xff9f,
        0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78, 0x9188, 0x81a9, 0xb1ca, 0xa1eb,
        0xd10c, 0xc12d, 0xf14e, 0xe16f, 0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046,
        0x6067, 0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e, 0x02b1, 0x1290,
        0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256, 0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e,
        0xe54f, 0xd52c, 0xc50d, 0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
        0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c, 0x26d3, 0x36f2, 0x0691,
        0x16b0, 0x6657, 0x7676, 0x4615, 0x5634, 0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9,
        0xb98a, 0xa9ab, 0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3, 0xcb7d,
        0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a, 0x4a75, 0x5a54, 0x6a37, 0x7a16,
        0x0af1, 0x1ad0, 0x2ab3, 0x3a92, 0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8,
        0x8dc9, 0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1, 0xef1f, 0xff3e,
        0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8, 0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93,
        0x3eb2, 0x0ed1, 0x1ef0
    };

    void crc16(std::uint16_t &crc, const void *data, const std::size_t size) {
        const std::uint8_t *cur = reinterpret_cast<const std::uint8_t *>(data);
        const std::uint8_t *end = reinterpret_cast<const std::uint8_t *>(data) + size;

        while (cur < end) {
            crc = (crc << 8) ^ (crc_tab[((crc >> 8) ^ *cur++) & 0xff]);
        }
    }

    static const std::string base64_ascii_map = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::size_t base64_encode(const std::uint8_t *source, const std::size_t source_size, char *dest,
        const std::size_t dest_max_size) {
        std::size_t dest_written = 0;

        for (std::size_t i = 0; i < source_size; i += 3) {
            const std::uint8_t b1 = source[i];
            const std::uint8_t b2 = (i + 1 > source_size) ? 0 : source[i + 1];
            const std::uint8_t b3 = (i + 2 > source_size) ? 0 : source[i + 2];

            if (dest) {
                if (dest_written == dest_max_size) {
                    return dest_written;
                }

                dest[dest_written] = base64_ascii_map[(b1 & 0b11111100) >> 2];
            }

            dest_written++;

            if (dest) {
                if (dest_written == dest_max_size) {
                    return dest_written;
                }

                dest[dest_written] = base64_ascii_map[((b1 & 0b11) << 4) | (b2 & 0b11110000) >> 4];
            }

            dest_written++;

            if (dest) {
                if (dest_written == dest_max_size) {
                    return dest_written;
                }

                dest[dest_written] = base64_ascii_map[((b2 & 0b1111) << 2) | (b3 & 0b11000000) >> 6];
            }

            dest_written++;

            if (dest) {
                if (dest_written == dest_max_size) {
                    return dest_written;
                }

                dest[dest_written] = base64_ascii_map[(b3 & 0b111111)];
            }

            dest_written++;
        }

        if (dest && source_size % 3 != 0) {
            for (std::size_t i = 0; i < 3 - source_size % 3; i++) {
                dest[dest_written - i - 1] = '=';
            }
        }

        return dest_written;
    }
    
    std::string base64_encode(const std::uint8_t *source, const std::size_t source_size) {
        // Answer by adzm : https://stackoverflow.com/questions/1533113/calculate-the-size-to-a-base-64-encoded-message
        std::string result;
        result.resize(((source_size + ((source_size % 3) ? (3 - (source_size % 3)) : 0)) / 3) * 4);

        base64_encode(source, source_size, result.data(), result.size());
        return result;
    }

    std::size_t base64_decode(const std::uint8_t *source, const std::size_t source_size, char *dest,
        const std::size_t dest_max_size) {
        std::size_t dest_written = 0;

        if (source_size % 4 != 0) {
            return dest_written;
        }

        for (std::size_t i = 0; i < source_size; i += 4) {
            const std::uint8_t b0 = static_cast<std::uint8_t>(base64_ascii_map.find(source[i]));
            const std::uint8_t b1 = static_cast<std::uint8_t>(base64_ascii_map.find(source[i + 1]));
            const std::uint8_t b2 = source[i + 2] == '=' ? 0 : static_cast<std::uint8_t>(base64_ascii_map.find(source[i + 2]));
            const std::uint8_t b3 = source[i + 3] == '=' ? 0 : static_cast<std::uint8_t>(base64_ascii_map.find(source[i + 3]));

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4309)
#endif
            if (b0 == static_cast<std::uint8_t>(std::string::npos) || b1 == static_cast<std::uint8_t>(std::string::npos) || b2 == static_cast<std::uint8_t>(std::string::npos) || b3 == static_cast<std::uint8_t>(std::string::npos)) {
                return dest_written;
            }

#ifdef _MSC_VER
#pragma warning(pop)
#endif

            if (dest) {
                if (dest_written == dest_max_size) {
                    return dest_written;
                }

                dest[dest_written] = ((b0 & 0b111111) << 2) | ((b1 & 0b00110000) >> 4);
            }

            dest_written++;

            if (dest) {
                if (dest_written == dest_max_size) {
                    return dest_written;
                }

                dest[dest_written] = ((b1 & 0b1111) << 4) | ((b2 & 0b00111100) >> 2);
            }

            dest_written++;

            if (dest) {
                if (dest_written == dest_max_size) {
                    return dest_written;
                }

                dest[dest_written] = ((b2 & 0b11) << 6) | (b3 & 0b00111111);
            }

            dest_written++;
        }

        for (std::size_t i = 0; i < 2; i++) {
            if (source[source_size - i - 1] == '=') {
                dest_written--;
            }
        }

        return dest_written;
    }

    static std::uint32_t calculate_checksum(const void *uids) {
        const std::uint8_t *cur = reinterpret_cast<decltype(cur)>(uids);
        const std::uint8_t *end = cur + 12;

        std::uint8_t buf[6];
        std::uint8_t *p = &buf[0];

        while (cur < end) {
            *p++ = (*cur);
            cur += 2;
        }

        std::uint16_t crc = 0;
        crypt::crc16(crc, &buf[0], 6);

        return crc;
    }

    std::uint32_t calculate_checked_uid_checksum(const std::uint32_t *uids) {
        return (calculate_checksum(reinterpret_cast<const std::uint8_t *>(uids) + 1) << 16) | calculate_checksum(uids);
    }

    imei_valid_error is_imei_valid(const std::string &supposed_imei) {
        if (supposed_imei.length() != 15) {
            return IMEI_ERROR_NO_RIGHT_LENGTH;
        }

        int sum = 0;

        for (std::size_t i = supposed_imei.length() - 1; i >= 1; i--) {
            if ((supposed_imei[i - 1] >= '0') && (supposed_imei[i - 1] <= '9')) {
                if (i % 2 == 0) {
                    int doubled = (supposed_imei[i - 1] - '0') * 2;
                    while (doubled != 0) {
                        sum += doubled % 10;
                        doubled /= 10;
                    }
                } else {
                    sum += (supposed_imei[i - 1] - '0');
                }
            } else {
                return IMEI_ERROR_INVALID_CHARACTER;
            }
        }

        if ((supposed_imei.back() >= '0') && (supposed_imei.back() <= '9')) {
            int rounded_sum = ((sum + 9) / 10) * 10;

            if ((rounded_sum - sum) != (supposed_imei.back() - '0')) {
                return IMEI_ERROR_INVALID_SUM;
            }
        } else {
            return IMEI_ERROR_INVALID_CHARACTER;
        }

        return IMEI_ERROR_NONE;
    }

    // ------------------------------------------------------------------------
    // SHA-256 (FIPS 180-4). Self-contained, public-domain style implementation.
    // ------------------------------------------------------------------------
    namespace {
        inline std::uint32_t rotr32(const std::uint32_t x, const std::uint32_t n) {
            return (x >> n) | (x << (32 - n));
        }

        const std::uint32_t k256[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        void sha256_block(std::uint32_t state[8], const std::uint8_t *p) {
            std::uint32_t w[64];
            for (int i = 0; i < 16; i++) {
                w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) | (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) | (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(p[i * 4 + 3]);
            }
            for (int i = 16; i < 64; i++) {
                const std::uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const std::uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }

            std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
            std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

            for (int i = 0; i < 64; i++) {
                const std::uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
                const std::uint32_t ch = (e & f) ^ (~e & g);
                const std::uint32_t t1 = h + s1 + ch + k256[i] + w[i];
                const std::uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
                const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                const std::uint32_t t2 = s0 + maj;
                h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
            }

            state[0] += a; state[1] += b; state[2] += c; state[3] += d;
            state[4] += e; state[5] += f; state[6] += g; state[7] += h;
        }
    }

    std::array<std::uint8_t, 32> sha256(const void *data, const std::size_t size) {
        std::uint32_t state[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };

        const std::uint8_t *p = reinterpret_cast<const std::uint8_t *>(data);
        std::size_t remaining = size;
        while (remaining >= 64) {
            sha256_block(state, p);
            p += 64;
            remaining -= 64;
        }

        // Final block(s): copy tail, append 0x80, pad, then 64-bit big-endian length.
        std::uint8_t tail[128] = { 0 };
        for (std::size_t i = 0; i < remaining; i++) {
            tail[i] = p[i];
        }
        tail[remaining] = 0x80;
        const std::size_t pad_blocks = (remaining < 56) ? 1 : 2;
        const std::uint64_t bit_len = static_cast<std::uint64_t>(size) * 8;
        const std::size_t len_off = pad_blocks * 64 - 8;
        for (int i = 0; i < 8; i++) {
            tail[len_off + i] = static_cast<std::uint8_t>(bit_len >> (56 - i * 8));
        }
        for (std::size_t b = 0; b < pad_blocks; b++) {
            sha256_block(state, tail + b * 64);
        }

        std::array<std::uint8_t, 32> digest{};
        for (int i = 0; i < 8; i++) {
            digest[i * 4] = static_cast<std::uint8_t>(state[i] >> 24);
            digest[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
            digest[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
            digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
        }
        return digest;
    }

    std::string sha256_hex(const void *data, const std::size_t size) {
        const std::array<std::uint8_t, 32> digest = sha256(data, size);
        static const char hex[] = "0123456789abcdef";
        std::string out(64, '0');
        for (int i = 0; i < 32; i++) {
            out[i * 2] = hex[digest[i] >> 4];
            out[i * 2 + 1] = hex[digest[i] & 0xf];
        }
        return out;
    }
}
