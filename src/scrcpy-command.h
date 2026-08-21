#pragma once

#include "scrcpy-settings.h"

#include <string>
#include <vector>

std::vector<std::string> build_adb_connect_args(const ScrcpySettings &settings);
std::vector<std::string> build_adb_selector_args(const ScrcpySettings &settings);
std::vector<std::string> build_scrcpy_probe_args(const ScrcpySettings &settings);
std::string parse_scrcpy_version_output(const std::string &output);
bool is_supported_scrcpy_server_version(const std::string &version);
std::string merge_realtime_video_codec_options(const std::string &user_options);
std::string normalize_video_codec_options(const std::string &value);
std::string normalize_audio_codec_options(const std::string &value);
std::string quote_for_log(const std::vector<std::string> &argv);
