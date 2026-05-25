#include "scrcpy-source.h"

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-scrcpy-source", "en-US")

namespace {

const char *scrcpy_source_get_name(void *)
{
  return obs_module_text("ScrcpySource");
}

void *scrcpy_source_create(obs_data_t *settings, obs_source_t *source)
{
  auto *context = new ScrcpySource(source);
  context->update(settings);
  return context;
}

void scrcpy_source_destroy(void *data)
{
  delete static_cast<ScrcpySource *>(data);
}

void scrcpy_source_update(void *data, obs_data_t *settings)
{
  static_cast<ScrcpySource *>(data)->update(settings);
}

uint32_t scrcpy_source_get_width(void *data)
{
  return static_cast<ScrcpySource *>(data)->width();
}

uint32_t scrcpy_source_get_height(void *data)
{
  return static_cast<ScrcpySource *>(data)->height();
}

void scrcpy_source_tick(void *data, float seconds)
{
  static_cast<ScrcpySource *>(data)->tick(seconds);
}

obs_source_info source_info = {};

} // namespace

bool obs_module_load(void)
{
  source_info.id = "obs_scrcpy_source";
  source_info.type = OBS_SOURCE_TYPE_INPUT;
  source_info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO;
  source_info.get_name = scrcpy_source_get_name;
  source_info.create = scrcpy_source_create;
  source_info.destroy = scrcpy_source_destroy;
  source_info.get_width = scrcpy_source_get_width;
  source_info.get_height = scrcpy_source_get_height;
  source_info.get_defaults = scrcpy_source_get_defaults;
  source_info.get_properties = scrcpy_source_get_properties;
  source_info.update = scrcpy_source_update;
  source_info.video_tick = scrcpy_source_tick;

  obs_register_source(&source_info);
  blog(LOG_INFO, "[obs-scrcpy-source] loaded");
  return true;
}
