#pragma once

#include "motor.hpp"

// Port of Tests/motor_test.py: spin forever at 400 steps/s
void run_motor_test(Motor& motor);

// Port of Tests/move_test.py: move to 200, wait 1 s, back to 0
void run_move_test(Motor& motor);
