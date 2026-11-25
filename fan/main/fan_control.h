#pragma once

#include <stdint.h>

constexpr uint8_t speed_from_percent(uint8_t percent)
{
    return (3U * percent + 99U) / 100U;
}

constexpr uint8_t percent_from_speed(uint8_t speed)
{
    return 100U * speed / 3U;
}

constexpr uint8_t next_speed(uint8_t speed, unsigned presses = 1)
{
    return (speed + presses % 4U) % 4U;
}

struct FanButton {
    bool sample(bool pressed, int64_t now_us)
    {
        if (pressed != candidate) {
            candidate = pressed;
            candidate_since = now_us;
        }
        if (candidate == stable || now_us - candidate_since < 30000) {
            return false;
        }
        stable = candidate;
        return stable;
    }

private:
    bool candidate = false;
    bool stable = false;
    int64_t candidate_since = 0;
};
