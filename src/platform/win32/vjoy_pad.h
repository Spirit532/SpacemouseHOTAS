#pragma once
#include "platform/platform.h"

VirtualPad* createVJoyPad();
// True if the vJoy driver is installed and its device interface is present.
bool isVJoyInstalled();
// Runs in the elevated helper process: writes this app's HID layout for `deviceId` and restarts the driver.
int configureVJoyElevated(int deviceId);
