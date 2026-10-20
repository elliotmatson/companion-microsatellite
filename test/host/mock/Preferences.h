// Preferences stand-in, backed by a process-wide map.
#pragma once

#include <Arduino.h>
#include <map>
#include <string>

class Preferences
{
public:
    static std::map<std::string, std::string> &store()
    {
        static std::map<std::string, std::string> values;
        return values;
    }

    bool begin(const char *name, bool) { _ns = std::string(name) + "/"; return true; }
    String getString(const char *key, const String &fallback)
    {
        auto it = store().find(_ns + key);
        return it == store().end() ? fallback : String(it->second.c_str());
    }
    uint32_t getUInt(const char *key, uint32_t fallback)
    {
        auto it = store().find(_ns + key);
        return it == store().end() ? fallback : static_cast<uint32_t>(std::stoul(it->second));
    }
    size_t putString(const char *key, const String &value)
    {
        store()[_ns + key] = value.c_str();
        return value.length();
    }
    size_t putUInt(const char *key, uint32_t value)
    {
        store()[_ns + key] = std::to_string(value);
        return 4;
    }

private:
    std::string _ns;
};
