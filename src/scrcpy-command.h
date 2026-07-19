#pragma once

#include "scrcpy-settings.h"

#include <string>
#include <vector>

std::vector<std::string> build_adb_connect_args(const ScrcpySettings &settings);
std::vector<std::string> build_adb_selector_args(const ScrcpySettings &settings);
std::vector<std::string> build_scrcpy_probe_args(const ScrcpySettings &settings);
std::string merge_realtime_video_codec_options(const std::string &user_options);
std::string normalize_video_codec_options(const std::string &value);
std::string normalize_audio_codec_options(const std::string &value);
std::string quote_for_log(const std::vector<std::string> &argv);
