// Minimal stand-ins for the parts of the Arduino-ESP32 core the library uses, so the
// Satellite classes can be exercised on a desktop machine. Not a general purpose mock.
#pragma once

#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

extern uint32_t g_mockMillis;
inline uint32_t millis()
{
    return g_mockMillis;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

extern bool g_mockVerbose;
__attribute__((format(printf, 3, 4))) inline void mockLog(char level, const char *tag, const char *format, ...)
{
    if (!g_mockVerbose && level != 'E')
    {
        return;
    }
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[%c][%s] ", level, tag);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}

#define ESP_LOGE(tag, format, ...) mockLog('E', tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) mockLog('W', tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) mockLog('I', tag, format, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) mockLog('D', tag, format, ##__VA_ARGS__)
#define ESP_LOGV(tag, format, ...) mockLog('V', tag, format, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// FreeRTOS
// ---------------------------------------------------------------------------

struct MockSemaphore
{
    std::recursive_mutex mutex;
};
using SemaphoreHandle_t = MockSemaphore *;
#define portMAX_DELAY 0xffffffffu
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex()
{
    return new MockSemaphore();
}
inline void vSemaphoreDelete(SemaphoreHandle_t s)
{
    delete s;
}
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t s, uint32_t)
{
    s->mutex.lock();
    return 1;
}
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t s)
{
    s->mutex.unlock();
    return 1;
}

typedef int esp_err_t;
#define ESP_OK 0

// ---------------------------------------------------------------------------
// ESP
// ---------------------------------------------------------------------------

struct MockEsp
{
    unsigned long long getEfuseMac() { return 0xa1b2c3d4e5f6ULL; }
};
extern MockEsp ESP;

// ---------------------------------------------------------------------------
// String
// ---------------------------------------------------------------------------

class String
{
public:
    String() = default;
    String(const char *value)
        : _s(value ? value : "") {}
    String(const String &) = default;
    String(String &&) = default;
    String &operator=(const String &) = default;
    String &operator=(String &&) = default;
    String &operator=(const char *value)
    {
        _s = value ? value : "";
        return *this;
    }

    template <typename T, typename = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, char> && !std::is_same_v<T, bool>>>
    explicit String(T value)
        : _s(std::to_string(value)) {}

    const char *c_str() const { return _s.c_str(); }
    unsigned int length() const { return static_cast<unsigned int>(_s.size()); }
    void reserve(unsigned int size) { _s.reserve(size); }

    bool concat(const char *value)
    {
        if (value)
            _s += value;
        return true;
    }
    bool concat(const char *value, unsigned int length)
    {
        _s.append(value, length);
        return true;
    }
    bool concat(const String &value)
    {
        _s += value._s;
        return true;
    }
    String &operator+=(const String &value)
    {
        _s += value._s;
        return *this;
    }
    String &operator+=(const char *value)
    {
        _s += value;
        return *this;
    }

    friend String operator+(const String &a, const String &b) { return String((a._s + b._s).c_str()); }
    friend String operator+(const String &a, const char *b) { return String((a._s + b).c_str()); }
    friend String operator+(const char *a, const String &b) { return String((a + b._s).c_str()); }

    bool operator==(const String &other) const { return _s == other._s; }
    bool operator==(const char *other) const { return _s == other; }
    bool operator!=(const String &other) const { return _s != other._s; }
    bool operator!=(const char *other) const { return _s != other; }
    bool operator<(const String &other) const { return _s < other._s; }
    char operator[](unsigned int index) const { return index < _s.size() ? _s[index] : 0; }

    String substring(unsigned int from, unsigned int to = 0xffffffffu) const
    {
        if (from > _s.size())
            return String();
        return String(_s.substr(from, to == 0xffffffffu ? std::string::npos : to - from).c_str());
    }
    long toInt() const { return atol(_s.c_str()); }
    void trim()
    {
        size_t start = _s.find_first_not_of(" \t\r\n");
        size_t end = _s.find_last_not_of(" \t\r\n");
        _s = start == std::string::npos ? "" : _s.substr(start, end - start + 1);
    }

private:
    std::string _s;
};
