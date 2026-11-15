// SPDX-License-Identifier: GPL-2.0-or-later
#include "HfsPlus.h"
#include <algorithm>
namespace HfsPlus {
namespace {
#include "HfsPlusUnicodeData.h"
template <class T, size_t N, size_t M> const T *Find(const T (&table)[N][M], uint32_t ch) {
    size_t lo = 0, hi = N;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (table[mid][0] < ch)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < N && table[lo][0] == ch ? table[lo] : nullptr;
}
unsigned Combining(char32_t c) {
    auto p = Find(combiningClasses, uint32_t(c));
    return p ? p[1] : 0;
}
void Decompose(char32_t c, std::u32string &out) {
    if (c >= 0xac00 && c <= 0xd7a3) {
        unsigned n = c - 0xac00;
        out += char32_t(0x1100 + n / 588);
        out += char32_t(0x1161 + (n % 588) / 28);
        if (n % 28)
            out += char32_t(0x11a7 + n % 28);
        return;
    }
    auto p = Find(decompositions, uint32_t(c));
    if (!p) {
        out += c;
        return;
    }
    Decompose(p[1], out);
    if (p[2])
        Decompose(p[2], out);
}
uint16_t Fold(uint16_t c) {
    auto p = Find(folds, c);
    return p ? p[1] : c;
}
} // namespace
std::u16string Normalize(const std::u16string &name) {
    std::u32string decomposed;
    std::u16string result;
    for (size_t i = 0; i < name.size(); ++i) {
        char32_t c = name[i];
        if (c >= 0xd800 && c <= 0xdbff) {
            if (i + 1 == name.size() || name[i + 1] < 0xdc00 || name[i + 1] > 0xdfff)
                throw Error("Invalid UTF-16 name");
            c = 0x10000 + ((c - 0xd800) << 10) + (name[++i] - 0xdc00);
        } else if (c >= 0xdc00 && c <= 0xdfff)
            throw Error("Invalid UTF-16 name");
        Decompose(c, decomposed);
    }
    for (size_t i = 1; i < decomposed.size(); ++i) {
        unsigned cc = Combining(decomposed[i]);
        if (!cc)
            continue;
        size_t j = i;
        while (j && Combining(decomposed[j - 1]) > cc) {
            std::swap(decomposed[j], decomposed[j - 1]);
            --j;
        }
    }
    for (char32_t c : decomposed) {
        if (c < 0x10000)
            result += char16_t(c);
        else {
            result += char16_t(0xd800 + ((c - 0x10000) >> 10));
            result += char16_t(0xdc00 + ((c - 0x10000) & 1023));
        }
    }
    if (result.empty() || result.size() > 255)
        throw Error("HFS+ names must contain 1 to 255 UTF-16 units after decomposition");
    if (result == u"." || result == u".." || result.find(u'/') != std::u16string::npos ||
        result.find(char16_t(0)) != std::u16string::npos)
        throw Error("Invalid HFS+ name");
    if (!CompareNames(result, u""))
        throw Error("HFS+ name consists only of ignorable characters");
    return result;
}
int CompareNames(const std::u16string &a, const std::u16string &b, bool sensitive) {
    size_t i = 0, j = 0;
    for (;;) {
        uint16_t x = 0, y = 0;
        while (i < a.size() && !x)
            x = sensitive ? uint16_t(a[i++]) : Fold(uint16_t(a[i++]));
        while (j < b.size() && !y)
            y = sensitive ? uint16_t(b[j++]) : Fold(uint16_t(b[j++]));
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            return 0;
    }
}
} // namespace HfsPlus
