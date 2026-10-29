#include "airplay-h264.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void expect(bool condition, const char *description)
{
  if (condition) {
    return;
  }
  std::cerr << description << " failed\n";
  std::exit(1);
}

} // namespace

int main()
{
  // 1. Geometry validation
  expect(airplay_is_valid_video_geometry(1920, 1080),
         "standard 1080p geometry is valid");
  expect(airplay_is_valid_video_geometry(1170, 2532),
         "iPhone portrait geometry is valid");
  expect(!airplay_is_valid_video_geometry(0, 1080),
         "zero width is rejected");
  expect(!airplay_is_valid_video_geometry(1920, 0),
         "zero height is rejected");
  expect(!airplay_is_valid_video_geometry(-1, 1080),
         "negative dimensions are rejected");
  expect(!airplay_is_valid_video_geometry(8193, 1080),
         "dimensions exceeding 8192 are rejected");
  expect(airplay_is_valid_video_geometry(8192, 8192),
         "total pixels equal to 64MP is permitted");
  expect(!airplay_is_valid_video_geometry(8192, 8193),
         "dimensions exceeding 8192 are rejected");

  // 2. Audio frame validation
  expect(airplay_is_valid_audio_frame(44100, 2, 16, 480 * 4),
         "standard 44.1kHz stereo 16-bit PCM is valid");
  expect(airplay_is_valid_audio_frame(48000, 2, 16, 960 * 4),
         "standard 48kHz stereo 16-bit PCM is valid");
  expect(!airplay_is_valid_audio_frame(44100, 2, 16, 0),
         "empty audio data is rejected");
  expect(!airplay_is_valid_audio_frame(44100, 2, 16, 7),
         "unaligned audio byte length (7 bytes) is rejected");
  expect(!airplay_is_valid_audio_frame(44100, 2, 16, 5),
         "unaligned audio byte length (5 bytes) is rejected");
  expect(!airplay_is_valid_audio_frame(4000, 2, 16, 16),
         "sub-8kHz audio rate is rejected");
  expect(!airplay_is_valid_audio_frame(256000, 2, 16, 16),
         "excessive audio sample rate is rejected");
  expect(!airplay_is_valid_audio_frame(44100, 3, 16, 24),
         "unsupported channel count is rejected");

  // 3. SPS / PPS parameter set detection
  // NAL unit 7 (SPS): 0x67, NAL unit 8 (PPS): 0x68
  const uint8_t sps_pps_payload[] = {
      0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1f,
      0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80};
  expect(airplay_h264_contains_sps(sps_pps_payload, sizeof(sps_pps_payload)),
         "SPS is found in parameter set packet");
  expect(airplay_h264_contains_pps(sps_pps_payload, sizeof(sps_pps_payload)),
         "PPS is found in parameter set packet");
  expect(!airplay_h264_contains_idr(sps_pps_payload, sizeof(sps_pps_payload)),
         "No IDR in parameter set packet");
  expect(airplay_is_h264_parameter_set_only(sps_pps_payload,
                                           sizeof(sps_pps_payload)),
         "Parameter set only packet is recognized");

  // NAL unit 5 (IDR): 0x65
  const uint8_t idr_payload[] = {
      0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80, 0x10, 0x20, 0x30};
  expect(airplay_h264_contains_idr(idr_payload, sizeof(idr_payload)),
         "IDR is found in keyframe packet");
  expect(!airplay_is_h264_parameter_set_only(idr_payload, sizeof(idr_payload)),
         "IDR packet is not parameter set only");

  // NAL unit 1 (non-IDR slice): 0x41
  const uint8_t non_idr_payload[] = {
      0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x12, 0x34};
  expect(!airplay_is_h264_parameter_set_only(non_idr_payload,
                                            sizeof(non_idr_payload)),
         "P-slice packet is not parameter set only");

  // Compound packet: SPS + PPS + IDR slice
  std::vector<uint8_t> compound_frame;
  compound_frame.insert(compound_frame.end(), sps_pps_payload,
                        sps_pps_payload + sizeof(sps_pps_payload));
  compound_frame.insert(compound_frame.end(), idr_payload,
                        idr_payload + sizeof(idr_payload));
  const std::vector<uint8_t> extracted =
      airplay_extract_sps_pps(compound_frame.data(), compound_frame.size());
  expect(extracted.size() == sizeof(sps_pps_payload),
         "airplay_extract_sps_pps extracts only SPS and PPS without slice data");
  expect(airplay_h264_contains_sps(extracted.data(), extracted.size()),
         "Extracted parameters contain SPS");
  expect(airplay_h264_contains_pps(extracted.data(), extracted.size()),
         "Extracted parameters contain PPS");
  expect(!airplay_h264_contains_idr(extracted.data(), extracted.size()),
         "Extracted parameters exclude IDR slice data");

  // 4. Packet preparation & SPS/PPS prepending
  const std::vector<uint8_t> cached_config(
      sps_pps_payload, sps_pps_payload + sizeof(sps_pps_payload));

  // IDR slice without SPS gets SPS/PPS prepended
  const std::vector<uint8_t> prep_idr = airplay_prepare_h264_packet(
      idr_payload, sizeof(idr_payload), 0, false, cached_config);
  expect(prep_idr.size() == sizeof(sps_pps_payload) + sizeof(idr_payload),
         "Prepared IDR packet contains prepended SPS/PPS");
  expect(airplay_h264_contains_sps(prep_idr.data(), prep_idr.size()),
         "Prepared packet contains SPS");
  expect(airplay_h264_contains_idr(prep_idr.data(), prep_idr.size()),
         "Prepared packet contains IDR");

  // Packet with in-band SPS is not double prepended
  const std::vector<uint8_t> prep_sps_pps = airplay_prepare_h264_packet(
      sps_pps_payload, sizeof(sps_pps_payload), 1, false, cached_config);
  expect(prep_sps_pps.size() == sizeof(sps_pps_payload),
         "Packet with in-band SPS is not duplicated");

  // P-slice packet without keyframe flag is not prepended
  const std::vector<uint8_t> prep_p = airplay_prepare_h264_packet(
      non_idr_payload, sizeof(non_idr_payload), 0, false, cached_config);
  expect(prep_p.size() == sizeof(non_idr_payload),
         "P-slice packet is unmodified");

  // P-slice packet when config_pending is true IS prepended
  const std::vector<uint8_t> prep_pending = airplay_prepare_h264_packet(
      non_idr_payload, sizeof(non_idr_payload), 0, true, cached_config);
  expect(prep_pending.size() ==
             sizeof(sps_pps_payload) + sizeof(non_idr_payload),
         "Pending config is prepended when requested");

  std::cout << "All airplay-h264 tests passed.\n";
  return 0;
}
