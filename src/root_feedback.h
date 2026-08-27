#pragma once

#include <stdbool.h>

/*
 * Ask Homebrew Channel's optional elevated service to clean legacy state and,
 * only when explicitly requested, prepare the exact-TV volatile Bluetooth
 * runtime. The bounded request runs before SDL or the video stack starts.
 * Returns true only when the requested root operation completed successfully.
 */
bool root_feedback_bootstrap(bool dualsense_runtime_requested);
