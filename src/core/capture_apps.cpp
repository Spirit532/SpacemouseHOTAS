#include "core/capture_apps.h"
#include <cctype>

std::vector<CaptureApp> defaultCaptureApps() {
    return {
        {"Wardogs", "wardogs"},
        {"Arma", "arma3|armareforger"},
        {"Battlefield", "battlefield|bf6|bf2042|bfv|bf1.exe|bf4.exe"},
        {"DCS World", "dcs.exe|dcs_mt.exe"},
        {"MSFS", "flightsimulator"},
        {"Squad", "squadgame"},
        {"Hell Let Loose", "hll-win64"},
        {"Post Scriptum", "postscriptum"},
        {"Ground Branch", "groundbranch"},
        {"Gray Zone Warfare", "gzwclient"},
        {"Escape from Tarkov", "escapefromtarkov"},
        {"Insurgency: Sandstorm", "insurgencyclient"},
        {"Ready or Not", "readyornot"},
        {"War Thunder", "aces.exe"},
        {"IL-2 Sturmovik", "il-2.exe"},
        {"Falcon BMS", "falcon bms"},
        {"Enlisted", "enlisted.exe"},
        {"Star Citizen", "starcitizen"},
        {"Elite Dangerous", "elitedangerous"},
    };
}

std::string toLower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

CaptureApp captureAppFromExecutable(const std::string& pathOrName) {
    size_t slash = pathOrName.find_last_of("/\\");
    std::string file = slash == std::string::npos ? pathOrName : pathOrName.substr(slash + 1);
    std::string name = file;
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return {name, toLower(file)};
}

static bool matches(const std::string& patterns, const std::string& processLower) {
    size_t start = 0;
    while (start <= patterns.size()) {
        size_t end = patterns.find('|', start);
        if (end == std::string::npos) end = patterns.size();
        std::string token = patterns.substr(start, end - start);
        if (!token.empty() && processLower.find(token) != std::string::npos) return true;
        start = end + 1;
    }
    return false;
}

const CaptureApp* findRunningCaptureApp(const std::vector<CaptureApp>& apps,
                                        const std::vector<std::string>& processes) {
    std::vector<std::string> lowered;
    lowered.reserve(processes.size());
    for (const std::string& process : processes) lowered.push_back(toLower(process));
    for (const CaptureApp& app : apps) {
        std::string patterns = toLower(app.match);
        for (const std::string& process : lowered)
            if (matches(patterns, process)) return &app;
    }
    return nullptr;
}
