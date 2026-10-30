// Ticker stand-in: the test calls fire() instead of a timer task.
#pragma once

#include <cstdint>
#include <functional>

class Ticker
{
public:
    void attach_ms(uint32_t, std::function<void()> callback) { _callback = std::move(callback); }
    void detach() { _callback = nullptr; }
    bool active() const { return static_cast<bool>(_callback); }
    void fire()
    {
        if (_callback)
            _callback();
    }

private:
    std::function<void()> _callback;
};
