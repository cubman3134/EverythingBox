/* A real, tiny libretro core — built from source, in-tree, as part of the test (issue #98).
 *
 * probe_customcore needs to inspect and run a core that is NOT in the catalogue and NOT downloaded from
 * anywhere: the whole feature is "load a core file the app has never heard of", and a probe that faked the
 * inspection with a hand-written struct would assert nothing about the part that can actually be wrong —
 * dlopen, the exports, retro_get_system_info's strings, and the SET_* a core declares during
 * retro_set_environment. So this file IS a libretro core: it exports the full entry-point set, draws a frame,
 * and answers the environment handshake.
 *
 * FOUR CORES IN ONE FILE. Which one it is is read from the environment variable EB_FIXTURE_CORE_MODE at each
 * call, so the probe can drive every case from one built artifact (a mode is read live, never cached, so
 * loading the same library again under a different mode gives a different core):
 *
 *   (unset) / "content"  a content-requiring core: claims ".ebf", does NOT declare supports_no_game.
 *   "nogame"             a game-engine core: claims no extensions, declares SET_SUPPORT_NO_GAME true.
 *   "badapi"             reports libretro API version 42 — a core this frontend cannot speak to.
 *   "needs"              asks for a Vulkan context and a camera: the "needs something we don't provide" case.
 *
 * Built as a MODULE library with its prefix cleared, into the probe's own output directory, so the probe
 * finds it by name. Nothing binary is committed — this source is the fixture.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "libretro.h"

#define FIX_W 64
#define FIX_H 48

static retro_environment_t   env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t  audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t    input_poll_cb;
static retro_input_state_t   input_state_cb;

static uint16_t framebuf[FIX_W * FIX_H];
static unsigned frame_no;

static const char* fix_mode(void)
{
   const char* m = getenv("EB_FIXTURE_CORE_MODE");
   return (m && *m) ? m : "content";
}

static int mode_is(const char* want)
{
   return strcmp(fix_mode(), want) == 0;
}

RETRO_API unsigned retro_api_version(void)
{
   /* A version this frontend does not speak — the "incompatible file" case, reported as a sentence. */
   return mode_is("badapi") ? 42u : (unsigned)RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info* info)
{
   memset(info, 0, sizeof(*info));
   if (mode_is("nogame"))
   {
      info->library_name     = "EB Fixture Engine";
      info->library_version  = "0.2";
      info->valid_extensions = NULL;         /* no content of its own */
   }
   else if (mode_is("needs"))
   {
      info->library_name     = "EB Fixture Greedy";
      info->library_version  = "0.3";
      info->valid_extensions = "ebv";
   }
   else
   {
      info->library_name     = "EB Fixture Content";
      info->library_version  = "0.1";
      /* Deliberately messy the way real cores are: mixed case, a leading dot, an empty segment. */
      info->valid_extensions = "EBF|.ebfixture||nes";
   }
   info->need_fullpath = 0;
   info->block_extract = 0;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info)
{
   memset(info, 0, sizeof(*info));
   info->geometry.base_width   = FIX_W;
   info->geometry.base_height  = FIX_H;
   info->geometry.max_width    = FIX_W;
   info->geometry.max_height   = FIX_H;
   info->geometry.aspect_ratio = (float)FIX_W / (float)FIX_H;
   info->timing.fps            = 60.0;
   info->timing.sample_rate    = 44100.0;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   env_cb = cb;
   if (!cb)
      return;

   if (mode_is("nogame"))
   {
      bool no_game = true;
      cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
   }
   else if (mode_is("needs"))
   {
      struct retro_hw_render_callback hw;
      memset(&hw, 0, sizeof(hw));
      hw.context_type = RETRO_HW_CONTEXT_VULKAN;
      cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw);
      {
         struct retro_camera_callback cam;
         memset(&cam, 0, sizeof(cam));
         cb(RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE, &cam);
      }
   }
   {
      enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
      cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
   }
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)      { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)        { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)            { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)          { input_state_cb = cb; }

RETRO_API void retro_init(void)   { frame_no = 0; }
RETRO_API void retro_deinit(void) { }
RETRO_API void retro_reset(void)  { frame_no = 0; }

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port; (void)device;
}

RETRO_API bool retro_load_game(const struct retro_game_info* game)
{
   /* The no-content contract, both directions, because the frontend has to get it exactly right:
    *   - a supports_no_game core is handed NULL (libretro's spec, and what RetroArch does), so this fixture
    *     REFUSES a non-NULL info in that mode. A frontend that passed a zeroed retro_game_info instead would
    *     look identical from the outside until a real game-engine core rejected it;
    *   - a content core is handed a real info block, so it refuses NULL. */
   if (mode_is("nogame"))
      return game == NULL;
   return game != NULL;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t num)
{
   (void)type; (void)info; (void)num;
   return false;
}

RETRO_API void retro_unload_game(void) { }

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API void retro_run(void)
{
   unsigned i;
   const uint16_t colour = (uint16_t)(0x001fu + (frame_no << 6));
   if (input_poll_cb)
      input_poll_cb();
   for (i = 0; i < FIX_W * FIX_H; ++i)
      framebuf[i] = colour;
   ++frame_no;
   if (video_cb)
      video_cb(framebuf, FIX_W, FIX_H, FIX_W * sizeof(uint16_t));
   if (audio_batch_cb)
   {
      static int16_t silence[735 * 2];
      audio_batch_cb(silence, 735);
   }
}

RETRO_API size_t retro_serialize_size(void)                     { return sizeof(frame_no); }
RETRO_API bool   retro_serialize(void* data, size_t size)
{
   if (size < sizeof(frame_no)) return false;
   memcpy(data, &frame_no, sizeof(frame_no));
   return true;
}
RETRO_API bool retro_unserialize(const void* data, size_t size)
{
   if (size < sizeof(frame_no)) return false;
   memcpy(&frame_no, data, sizeof(frame_no));
   return true;
}

RETRO_API void  retro_cheat_reset(void) { }
RETRO_API void  retro_cheat_set(unsigned index, bool enabled, const char* code)
{
   (void)index; (void)enabled; (void)code;
}

RETRO_API void*  retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
