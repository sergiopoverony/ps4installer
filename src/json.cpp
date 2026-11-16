// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#include "json.h"

#include <cstdio>

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += (char)c;  // UTF-8 передаём как есть
                }
        }
    }
    return o;
}

static void append_utf8(std::string* o, unsigned cp) {
    if (cp < 0x80) {
        *o += (char)cp;
    } else if (cp < 0x800) {
        *o += (char)(0xC0 | (cp >> 6));
        *o += (char)(0x80 | (cp & 0x3F));
    } else {
        *o += (char)(0xE0 | (cp >> 12));
        *o += (char)(0x80 | ((cp >> 6) & 0x3F));
        *o += (char)(0x80 | (cp & 0x3F));
    }
}

// Читает строку JSON, начиная с открывающей кавычки; pos указывает на неё. Возвращает false при ошибке.
static bool read_string(const std::string& s, size_t* pos, std::string* out) {
    size_t i = *pos + 1;
    out->clear();
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') {
            *pos = i + 1;
            return true;
        }
        if (c == '\\') {
            if (++i >= s.size()) {
                return false;
            }
            switch (s[i]) {
                case '"': *out += '"'; break;
                case '\\': *out += '\\'; break;
                case '/': *out += '/'; break;
                case 'n': *out += '\n'; break;
                case 'r': *out += '\r'; break;
                case 't': *out += '\t'; break;
                case 'u': {
                    if (i + 4 >= s.size()) {
                        return false;
                    }
                    unsigned cp = 0;
                    for (int k = 1; k <= 4; k++) {
                        char h = s[i + k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= h - '0';
                        else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                        else return false;
                    }
                    append_utf8(out, cp);
                    i += 4;
                    break;
                }
                default: return false;
            }
        } else {
            *out += c;
        }
        i++;
    }
    return false;
}

static void skip_ws(const std::string& s, size_t* i) {
    while (*i < s.size() && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\n' || s[*i] == '\r')) {
        (*i)++;
    }
}

bool json_get_string_array(const std::string& body, const std::string& key, std::vector<std::string>* out) {
    std::string needle = "\"" + key + "\"";
    size_t p = body.find(needle);
    if (p == std::string::npos) {
        return false;
    }
    size_t i = p + needle.size();
    skip_ws(body, &i);
    if (i >= body.size() || body[i] != ':') {
        return false;
    }
    i++;
    skip_ws(body, &i);
    if (i >= body.size() || body[i] != '[') {
        return false;
    }
    i++;
    out->clear();
    for (;;) {
        skip_ws(body, &i);
        if (i >= body.size()) {
            return false;
        }
        if (body[i] == ']') {
            return true;
        }
        if (body[i] != '"') {
            return false;
        }
        std::string item;
        if (!read_string(body, &i, &item)) {
            return false;
        }
        out->push_back(item);
        skip_ws(body, &i);
        if (i < body.size() && body[i] == ',') {
            i++;
        }
    }
}
