// Headless check that libmpv opens an audio file and exposes the properties the MpvWidget "now playing"
// overlay relies on: a media-title, a real duration, and NO video width (so it's treated as audio-only).
#include <mpv/client.h>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: probe_audio <file>\n"); return 2; }

    mpv_handle* mpv = mpv_create();
    mpv_set_option_string(mpv, "ao", "null");      // decode without needing a real output device
    mpv_set_option_string(mpv, "vo", "null");
    if (mpv_initialize(mpv) < 0) { printf("mpv init failed\n"); return 1; }

    const char* cmd[] = { "loadfile", argv[1], nullptr };
    mpv_command(mpv, cmd);

    bool loaded = false;
    for (int i = 0; i < 500 && !loaded; ++i)
    {
        mpv_event* e = mpv_wait_event(mpv, 0.05);
        if (e->event_id == MPV_EVENT_FILE_LOADED) loaded = true;
        else if (e->event_id == MPV_EVENT_END_FILE) break;
    }
    if (!loaded) { printf("file did not load\n"); mpv_terminate_destroy(mpv); return 1; }

    double duration = 0; mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration);
    int64_t width = -1;  int wrc = mpv_get_property(mpv, "width", MPV_FORMAT_INT64, &width);
    char* title = nullptr; mpv_get_property(mpv, "media-title", MPV_FORMAT_STRING, &title);
    char* acodec = nullptr; mpv_get_property(mpv, "audio-codec-name", MPV_FORMAT_STRING, &acodec);

    printf("loaded ok\n");
    printf("media-title: %s\n", title ? title : "(none)");
    printf("duration:    %.2fs\n", duration);
    printf("audio-codec: %s\n", acodec ? acodec : "(none)");
    printf("video width: %s\n", wrc < 0 ? "none (audio-only)" : "present");
    const bool ok = title && duration > 0 && wrc < 0 && acodec;
    printf("%s\n", ok ? "AUDIO PLAYS: libmpv decodes audio + exposes overlay metadata" : "unexpected");

    if (title) mpv_free(title);
    if (acodec) mpv_free(acodec);
    mpv_terminate_destroy(mpv);
    return ok ? 0 : 1;
}
