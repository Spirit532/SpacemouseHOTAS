// Command-line developer tools; each returns a process exit code.
#pragma once
#include <string>

// Replays steps ("x,y" click, "h:x,y" hover, "k:vk" key, "t:text" typing) and saves the UI as a 24-bit BMP.
int runScreenshot(const wchar_t* bmpPath, int scale, wchar_t** steps, int stepCount, const std::string& configPath);
// Sends known values to vJoy and reads them back through the WinMM joystick API.
int runVJoySelfTest(const wchar_t* reportPath);
// Writes the app icon as a multi-size .ico (used for the exe resource).
int writeIconFile(const wchar_t* icoPath);
