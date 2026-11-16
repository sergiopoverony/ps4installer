// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#include "sha256.h"
#include <stdio.h>
#include <string.h>

static const unsigned int K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

static unsigned int rotr(unsigned int x, int n) { return (x >> n) | (x << (32 - n)); }

Sha256::Sha256() : len(0), fill(0)
{
    static const unsigned int init[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(h, init, sizeof(h));
}

void Sha256::block(const unsigned char* p)
{
    unsigned int w[64];
    for (int i = 0; i < 16; i++) w[i] = ((unsigned)p[i*4] << 24) | ((unsigned)p[i*4+1] << 16) | ((unsigned)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        unsigned int s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        unsigned int s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    unsigned int a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        unsigned int S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        unsigned int ch = (e & f) ^ (~e & g);
        unsigned int t1 = hh + S1 + ch + K[i] + w[i];
        unsigned int S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        unsigned int mj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void Sha256::update(const void* data, size_t n)
{
    const unsigned char* p = (const unsigned char*)data;
    len += n;
    while (n > 0) {
        size_t take = 64 - fill < n ? 64 - fill : n;
        memcpy(buf + fill, p, take);
        fill += take; p += take; n -= take;
        if (fill == 64) { block(buf); fill = 0; }
    }
}

std::string Sha256::hex()
{
    unsigned long long bits = len * 8;
    unsigned char pad = 0x80;
    update(&pad, 1);
    unsigned char z = 0;
    while (fill != 56) update(&z, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (unsigned char)(bits >> (56 - 8 * i));
    update(lb, 8);
    static const char* D = "0123456789abcdef";
    std::string o;
    for (int i = 0; i < 8; i++) for (int k = 28; k >= 0; k -= 4) o += D[(h[i] >> k) & 15];
    return o;
}

std::string sha256_file(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    Sha256 s;
    unsigned char b[65536];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.update(b, n);
    fclose(f);
    return s.hex();
}
