// Config file: plain "key=value" lines. Unknown keys are ignored, missing keys keep their defaults.
#pragma once
#include <vector>
#include "core/capture_apps.h"
#include "core/mapping.h"

// Returns false if the file could not be read. `apps` is replaced only if the file stores a list.
bool loadConfig(const char* path, Profile& profile, std::vector<CaptureApp>& apps);
bool saveConfig(const char* path, const Profile& profile, const std::vector<CaptureApp>& apps);
