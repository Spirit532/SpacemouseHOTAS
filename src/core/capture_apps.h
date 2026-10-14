// Apps that trigger 3D mouse capture while they are running.
#pragma once
#include <string>
#include <vector>

struct CaptureApp {
    std::string name;   // display name
    std::string match;  // '|'-separated, case-insensitive substrings of the executable file name
    bool operator==(const CaptureApp& o) const { return name == o.name && match == o.match; }
};

std::vector<CaptureApp> defaultCaptureApps();
// Builds an entry from a path or file name: "C:\Games\DCS.exe" -> {"DCS", "dcs.exe"}.
CaptureApp captureAppFromExecutable(const std::string& pathOrName);
// Returns the first app matched by any of the running process names, or nullptr.
const CaptureApp* findRunningCaptureApp(const std::vector<CaptureApp>& apps, const std::vector<std::string>& processes);
std::string toLower(std::string s);
