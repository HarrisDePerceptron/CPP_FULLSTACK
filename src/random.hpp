#pragma once

#include <sodium.h>

#include <array>
#include <string>

namespace hello {

template<std::size_t Bytes>
std::string randomHex() {
    std::array<unsigned char, Bytes> bytes;
    std::array<char, Bytes * 2 + 1> hex;
    randombytes_buf(bytes.data(), bytes.size());
    sodium_bin2hex(hex.data(), hex.size(), bytes.data(), bytes.size());
    return {hex.data(), hex.size() - 1};
}

}
