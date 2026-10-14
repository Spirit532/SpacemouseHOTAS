#include "platform/linux/system_linux.h"
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

static std::string configHome() {
    if (const char* xdg = getenv("XDG_CONFIG_HOME")) return xdg;
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/.config";
}

std::string configFilePath() {
    std::string directory = configHome();
    mkdir(directory.c_str(), 0755);
    directory += "/spacemousehotas";
    mkdir(directory.c_str(), 0755);
    return directory + "/config.ini";
}

void setStartWithLinux(bool enabled) {
    std::string directory = configHome() + "/autostart";
    std::string path = directory + "/spacemousehotas.desktop";
    if (!enabled) {
        remove(path.c_str());
        return;
    }
    char executable[4096];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof executable - 1);
    if (length <= 0) return;
    executable[length] = 0;
    mkdir(directory.c_str(), 0755);
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    fprintf(f,
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=SpacemouseHOTAS\n"
            "Comment=SpaceMouse to virtual HOTAS\n"
            "Exec=\"%s\"\n"
            "Terminal=false\n"
            "X-GNOME-Autostart-enabled=true\n",
            executable);
    fclose(f);
}
