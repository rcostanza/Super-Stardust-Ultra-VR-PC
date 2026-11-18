/* game/release.txt: the stamp stardust-setup writes once it has prepared game/ (setup/setup.cpp)
 * and that stardust checks at every start (src/launcher.c). It names the release (release.txt's
 * first line), the SHA-256 of the dump's eboot.bin and that file's size and last write time:
 *     release=<release.txt's first line>
 *     dump=<sha256 of eboot.bin>
 *     eboot=<size> <time>
 * game/ is current while the release is the same and eboot.bin is the same file (size and time, the
 * quick check) or has the same contents (SHA-256, when the quick check fails: a copy of the dump).
 * Otherwise (a newer release unpacked over the folder, another dump) game/ is prepared again, and
 * the setup first warns when the dump is not the one this release was tested with.
 * Header only, for the launcher (C) and the setup (C++). Paths are UTF-8. */
#ifndef STARDUST_PREPARED_STAMP_H
#define STARDUST_PREPARED_STAMP_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define STARDUST_STAMP_SEP "\\"
#else
#include <sys/stat.h>
#define STARDUST_STAMP_SEP "/"
#endif

#define STARDUST_STAMP_CHARS 1024

/* fopen for a UTF-8 path (Windows: through the wide API, whatever the code page). */
static inline FILE *stardust_stamp_open(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wide[MAX_PATH * 4], wide_mode[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH * 4) ||
        !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wide_mode, 8)) return NULL;
    return _wfopen(wide, wide_mode);
#else
    return fopen(path, mode);
#endif
}

/* "<size> <last write time>" of a file: Windows' FILETIME, Linux's seconds.nanoseconds (as GNU
 * stat -c '%s %.9Y' prints them). 0 when it is not a file. */
static inline int stardust_stamp_file(const char *path, char *out, size_t size) {
#ifdef _WIN32
    wchar_t wide[MAX_PATH * 4];
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH * 4) ||
        !GetFileAttributesExW(wide, GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    unsigned long long bytes = ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    unsigned long long time = ((unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
    snprintf(out, size, "%llu %llu", bytes, time);
#else
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return 0;
    snprintf(out, size, "%llu %llu.%09ld", (unsigned long long)st.st_size, (unsigned long long)st.st_mtim.tv_sec,
             (long)st.st_mtim.tv_nsec);
#endif
    return 1;
}

static inline uint32_t stardust_rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

/* One 64-byte block of FIPS 180-4 SHA-256 into the state h. */
static inline void stardust_sha256_block(uint32_t h[8], const unsigned char *b) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 | (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = stardust_rotr(w[i - 15], 7) ^ stardust_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = stardust_rotr(w[i - 2], 17) ^ stardust_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = hh + (stardust_rotr(e, 6) ^ stardust_rotr(e, 11) ^ stardust_rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (stardust_rotr(a, 2) ^ stardust_rotr(a, 13) ^ stardust_rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* SHA-256 of a file as 64 lower-case hex digits in out[65]; 0 when it cannot be read. */
static inline int stardust_sha256_file(const char *path, char out[65]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    FILE *f = stardust_stamp_open(path, "rb");
    if (!f) return 0;
    unsigned char *buf = (unsigned char *)malloc(1 << 20);
    if (!buf) { fclose(f); return 0; }
    unsigned long long total = 0;
    size_t n, kept = 0; /* bytes of a block not yet hashed, at the start of buf */
    while ((n = fread(buf + kept, 1, (1 << 20) - kept, f)) > 0) {
        total += n;
        n += kept;
        size_t full = n - n % 64;
        for (size_t at = 0; at < full; at += 64) stardust_sha256_block(h, buf + at);
        kept = n - full;
        memmove(buf, buf + full, kept);
    }
    int ok = !ferror(f);
    fclose(f);
    if (ok) {
        /* Padding: 0x80, zeros, then the bit length, big-endian, in one or two final blocks. */
        unsigned char tail[128] = {0};
        memcpy(tail, buf, kept);
        tail[kept] = 0x80;
        size_t tail_size = kept + 9 <= 64 ? 64 : 128;
        for (int i = 0; i < 8; ++i) tail[tail_size - 1 - i] = (unsigned char)((total * 8) >> (i * 8));
        for (size_t at = 0; at < tail_size; at += 64) stardust_sha256_block(h, tail + at);
        for (int i = 0; i < 64; ++i) out[i] = "0123456789abcdef"[(h[i / 8] >> (28 - 4 * (i % 8))) & 15];
        out[64] = 0;
    }
    free(buf);
    return ok;
}

/* release.txt's first line in `root`; 0 when it is missing or empty. */
static inline int stardust_stamp_release(const char *root, char *out, size_t size) {
    char path[4096 + 64];
    snprintf(path, sizeof path, "%s" STARDUST_STAMP_SEP "release.txt", root);
    FILE *f = stardust_stamp_open(path, "rb");
    if (!f) return 0;
    if (!fgets(out, (int)size, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
    return out[0] != 0;
}

/* The stamp for the release folder `root` with the dump `game`; 0 when release.txt or eboot.bin
 * cannot be read. */
static inline int stardust_prepared_stamp(const char *root, const char *game, char *out, size_t size) {
    char path[4096 + 64], release[256], sha[65], eboot[64];
    if (!stardust_stamp_release(root, release, sizeof release)) return 0;
    snprintf(path, sizeof path, "%s" STARDUST_STAMP_SEP "eboot.bin", game);
    if (!stardust_stamp_file(path, eboot, sizeof eboot) || !stardust_sha256_file(path, sha)) return 0;
    snprintf(out, size, "release=%s\ndump=%s\neboot=%s\n", release, sha, eboot);
    return 1;
}

/* The value of `key`= in a stamp, copied to out; 0 when the stamp has no such line. */
static inline int stardust_stamp_value(const char *stamp, const char *key, char *out, size_t size) {
    size_t key_len = strlen(key);
    for (const char *line = stamp; *line;) {
        size_t len = strcspn(line, "\r\n");
        if (len > key_len && !strncmp(line, key, key_len) && line[key_len] == '=') {
            snprintf(out, size, "%.*s", (int)(len - key_len - 1), line + key_len + 1);
            return 1;
        }
        line += len;
        line += strspn(line, "\r\n");
    }
    return 0;
}

/* 1 when `root`/game/release.txt is the stamp of this release and of this dump's eboot.bin. */
static inline int stardust_prepared(const char *root, const char *game) {
    char have[STARDUST_STAMP_CHARS], path[4096 + 64], release[256], value[256], eboot[64], sha[65];
    snprintf(path, sizeof path, "%s" STARDUST_STAMP_SEP "game" STARDUST_STAMP_SEP "release.txt", root);
    FILE *f = stardust_stamp_open(path, "rb");
    if (!f) return 0;
    size_t n = fread(have, 1, sizeof have - 1, f);
    fclose(f);
    have[n] = 0;
    if (!stardust_stamp_release(root, release, sizeof release) || !stardust_stamp_value(have, "release", value, sizeof value) ||
        strcmp(value, release))
        return 0;
    snprintf(path, sizeof path, "%s" STARDUST_STAMP_SEP "eboot.bin", game);
    if (!stardust_stamp_file(path, eboot, sizeof eboot)) return 0;
    if (stardust_stamp_value(have, "eboot", value, sizeof value) && !strcmp(value, eboot)) return 1;
    return stardust_stamp_value(have, "dump", value, sizeof value) && stardust_sha256_file(path, sha) && !strcmp(value, sha);
}

#endif
