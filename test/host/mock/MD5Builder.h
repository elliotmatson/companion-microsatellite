// MD5Builder stand-in - not MD5, just a stable 32 hex digit digest of the input.
#pragma once

#include <Arduino.h>
#include <cstdio>
#include <string>

class MD5Builder
{
public:
    void begin() { _data.clear(); }
    void add(const String &value) { _data += value.c_str(); }
    void calculate() {}
    String toString() const
    {
        uint64_t a = 1469598103934665603ULL, b = 1099511628211ULL;
        for (char c : _data)
        {
            a = (a ^ static_cast<uint8_t>(c)) * 1099511628211ULL;
            b = (b ^ static_cast<uint8_t>(c)) * 1469598103934665603ULL + 7;
        }
        char hex[33];
        snprintf(hex, sizeof(hex), "%016llx%016llx", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
        return String(hex);
    }

private:
    std::string _data;
};
