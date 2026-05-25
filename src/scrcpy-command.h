#pragma once

#include "scrcpy-settings.h"

#include <string>
#include <vector>

std::vector<std::string> build_scrcpy_probe_args(const ScrcpySettings &settings);
std::string quote_for_log(const std::vector<std::string> &argv);

