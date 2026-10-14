// Windows services behind PlatformHooks, plus string conversion helpers.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

std::string toUtf8(const wchar_t* text);
std::wstring toWide(const std::string& text);

void injectKey(int vk, bool down);
void injectMouseButton(int button, bool down);
void listProcesses(std::vector<std::string>& executableNames);
bool browseForExecutable(std::string& path);
void setStartWithWindows(bool enabled);
void setDialogOwner(HWND window);  // parent for the file dialog
