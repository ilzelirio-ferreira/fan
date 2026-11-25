#include <cassert>
#include "../main/fan_control.h"

int main()
{
    for (uint8_t speed = 0; speed <= 3; ++speed) {
        assert(next_speed(speed) == (speed + 1) % 4);
    }
    assert(next_speed(3, 2) == 1);
    assert(percent_from_speed(0) == 0);
    assert(percent_from_speed(1) == 33);
    assert(percent_from_speed(2) == 66);
    assert(percent_from_speed(3) == 100);

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
