#include <cassert>
#include "../main/fan_control.h"

int main()
{
    for (uint8_t speed = 0; speed <= 3; ++speed) {
        assert(next_speed(speed) == (speed + 1) % 4);
        assert(speed_from_percent(percent_from_speed(speed)) == speed);
    }
    assert(speed_from_percent(1) == 1);
    assert(speed_from_percent(33) == 1);
    assert(speed_from_percent(34) == 2);
    assert(speed_from_percent(66) == 2);
    assert(speed_from_percent(67) == 3);
    assert(speed_from_percent(100) == 3);

    FanButton button;
    assert(!button.sample(true, 0));
    assert(!button.sample(false, 10000));
    assert(!button.sample(true, 20000));
    assert(!button.sample(true, 49999));
    assert(button.sample(true, 50000));
    assert(!button.sample(true, 500000));
    assert(!button.sample(false, 510000));
    assert(!button.sample(false, 540000));
    assert(!button.sample(true, 550000));
    assert(button.sample(true, 580000));
}
