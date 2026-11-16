// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#pragma once
// SHA-256 (для проверки скачанных заплаток каталога).
#include <string>

struct Sha256 {
    unsigned int h[8];
    unsigned long long len;
    unsigned char buf[64];
    size_t fill;
    Sha256();
    void update(const void* data, size_t n);
    std::string hex();   // 64 символа, нижний регистр; после вызова объект больше не используется
private:
    void block(const unsigned char* p);
};

std::string sha256_file(const std::string& path);   // пусто, если файл не открылся
