#include "app.h"
#include "config.h"
#include "session_priv.h"

#include "stream/session.h"
#include "app_settings.h"

#include "backend/pcmanager/priv.h"

#include <Limelight.h>

#include "libgamestream/errors.h"

#include "logging.h"
#include "ss4s.h"
#include "input/input_gamepad.h"
#include "app_session.h"
#include "session_worker.h"
#include "stream/input/session_virt_mouse.h"

// Expected luminance values in SEI are in units of 0.0001 cd/m2
#define LUMINANCE_SCALE 10000

int streaming_errno = GS_OK;
char streaming_errmsg[1024];

static bool streaming_sops_supported(PDISPLAY_MODE modes, int w, int h, int fps);

static void session_config_init(app_t *app, session_config_t *config, const SERVER_DATA *server,
                                const CONFIGURATION *app_config);

static void populate_hdr_info_vui(SS4S_VideoHDRInfo *info, const STREAM_CONFIGURATION *config);

session_t *session_create(app_t *app, const CONFIGURATION *config, const SERVER_DATA *server, const APP_LIST *gs_app) {
    session_t *session = malloc(sizeof(session_t));
    SDL_memset(session, 0, sizeof(session_t));
    session_config_init(app, &session->config, server, config);
    session->app = app;
    session->display_width = app->ui.width;
    session->display_height = app->ui.height;
    session->audio_cap = app->ss4s.audio_cap;
    session->video_cap = app->ss4s.video_cap;
    session->server = serverdata_clone(server);
    // The flags seem to be the same to supportedVideoFormats, use it for now...
    session->server->serverInfo.serverCodecModeSupport = 0;
    if (session->config.stream.supportedVideoFormats & VIDEO_FORMAT_H264) {
        session->server->serverInfo.serverCodecModeSupport |= SCM_H264;
    }
    if (session->config.stream.supportedVideoFormats & VIDEO_FORMAT_H265) {
        session->server->serverInfo.serverCodecModeSupport |= SCM_HEVC;
        if (session->config.stream.supportedVideoFormats & VIDEO_FORMAT_H265_MAIN10) {
            session->server->serverInfo.serverCodecModeSupport |= SCM_HEVC_MAIN10;
        }
    }
    if (session->config.stream.supportedVideoFormats & VIDEO_FORMAT_AV1_MAIN8) {
        session->server->serverInfo.serverCodecModeSupport |= SCM_AV1_MAIN8;
    }
    if (session->config.stream.supportedVideoFormats & VIDEO_FORMAT_AV1_MAIN10) {
        session->server->serverInfo.serverCodecModeSupport |= SCM_AV1_MAIN10;
    }
    session->app_id = gs_app->id;
    session->app_name = strdup(gs_app->name);
    session->mutex = SDL_CreateMutex();
    session->state_lock = SDL_CreateMutex();
    session->cond = SDL_CreateCond();
#if FEATURE_EMBEDDED_SHELL
    if (!app_is_decoder_valid(app)) {
        session->embed = app_has_embedded(session->app);
    }
#endif
    session_input_init(&session->input, session, &app->input, &session->config);
    SDL_ThreadFunction worker_fn = (SDL_ThreadFunction) session_worker;
#if FEATURE_EMBEDDED_SHELL
    if (session_use_embedded(session)) {
        worker_fn = (SDL_ThreadFunction) session_worker_embedded;
    }
#endif
    session->thread = SDL_CreateThread(worker_fn, "session", session);
    return session;
}

void session_destroy(session_t *session) {
    session_interrupt(session, false, STREAMING_INTERRUPT_QUIT);
    session_input_deinit(&session->input);
    SDL_WaitThread(session->thread, NULL);
    serverdata_free(session->server);
    SDL_DestroyCond(session->cond);
    SDL_DestroyMutex(session->mutex);
    SDL_DestroyMutex(session->state_lock);
    free(session->app_name);
    free(session);
}

void session_interrupt(session_t *session, bool quitapp, streaming_interrupt_reason_t reason) {
    if (!session) {
        return;
    }
    SDL_LockMutex(session->mutex);
    if (session->interrupted) {
        SDL_UnlockMutex(session->mutex);
        return;
    }
    session_input_interrupt(&session->input);
    session->quitapp = quitapp;
    session->interrupted = true;
#if FEATURE_EMBEDDED_SHELL
    if (session->embed && session->embed_process) {
        embed_interrupt(session->embed_process);
    }
#endif
    if (reason >= STREAMING_INTERRUPT_ERROR) {
        switch (reason) {
            case STREAMING_INTERRUPT_WATCHDOG:
                streaming_error(session, reason, "Stream stalled");
                break;
            case STREAMING_INTERRUPT_NETWORK:
                streaming_error(session, reason, "Network error happened");
                break;
            case STREAMING_INTERRUPT_DECODER:
                streaming_error(session, reason, "Decoder reported error");
                break;
            default:
                streaming_error(session, reason, "Error occurred while in streaming");
                break;
        }
    }
    SDL_CondSignal(session->cond);
    SDL_UnlockMutex(session->mutex);
    SDL_UnlockMutex(session->state_lock);
}

bool session_accepting_input(session_t *session) {
    return session->input.started && !ui_should_block_input();
}

bool session_start_input(session_t *session) {
#if FEATURE_EMBEDDED_SHELL
    if (session->embed) {
        return false;
    }
#endif
    session_input_started(&session->input);
    return true;
}

void session_stop_input(session_t *session) {
    session_input_stopped(&session->input);
}

bool session_has_input(session_t *session) {
    return session->input.started;
}

void session_toggle_vmouse(session_t *session) {
    bool value = session->config.vmouse && !session_input_is_vmouse_active(&session->input.vmouse);
    session_input_set_vmouse_active(&session->input.vmouse, value);
}

void session_screen_keyboard_opened(session_t *session) {
    session_input_screen_keyboard_opened(&session->input);
}

void session_screen_keyboard_closed(session_t *session) {
    session_input_screen_keyboard_closed(&session->input);
}

void streaming_display_size(session_t *session, short width, short height) {
    session->display_width = width;
    session->display_height = height;
}

void streaming_enter_fullscreen(session_t *session) {
    if (!session->player) {
        return;
    }
    if ((session->video_cap.transform & SS4S_VIDEO_CAP_TRANSFORM_UI_COMPOSITING) == 0) {
        SS4S_PlayerVideoSetDisplayArea(session->player, NULL, NULL);
    }
}

void streaming_enter_overlay(session_t *session, int x, int y, int w, int h) {
    app_set_mouse_grab(&session->app->input, false);
    SS4S_VideoRect dst = {x, y, w, h};
    if ((session->video_cap.transform & SS4S_VIDEO_CAP_TRANSFORM_UI_COMPOSITING) == 0) {
        SS4S_PlayerVideoSetDisplayArea(session->player, NULL, &dst);
    }
}

void streaming_set_hdr(session_t *session, bool hdr) {
    commons_log_info("Session", "HDR is %s", hdr ? "enabled" : "disabled");
    SS_HDR_METADATA hdr_metadata;
    if (!hdr) {
        SS4S_PlayerVideoSetHDRInfo(session->player, NULL);
    } else if (LiGetHdrMetadata(&hdr_metadata)) {
        SS4S_VideoHDRInfo info = {
                .displayPrimariesX = {
                        hdr_metadata.displayPrimaries[0].x,
                        hdr_metadata.displayPrimaries[1].x,
                        hdr_metadata.displayPrimaries[2].x
                },
                .displayPrimariesY = {
                        hdr_metadata.displayPrimaries[0].y,
                        hdr_metadata.displayPrimaries[1].y,
                        hdr_metadata.displayPrimaries[2].y
                },
                .whitePointX = hdr_metadata.whitePoint.x,
                .whitePointY = hdr_metadata.whitePoint.y,
                .maxDisplayMasteringLuminance = hdr_metadata.maxDisplayLuminance * LUMINANCE_SCALE,
                .minDisplayMasteringLuminance = hdr_metadata.minDisplayLuminance,
                .maxContentLightLevel = hdr_metadata.maxContentLightLevel,
                .maxPicAverageLightLevel = hdr_metadata.maxFrameAverageLightLevel,
        };
        populate_hdr_info_vui(&info, &session->config.stream);
        SS4S_PlayerVideoSetHDRInfo(session->player, &info);
    } else {
        SS4S_VideoHDRInfo info = {
                .displayPrimariesX = {34000, 13250, 7500},
                .displayPrimariesY = {16000, 34500, 3000},
                .whitePointX = 15635,
                .whitePointY = 16450,
                .maxDisplayMasteringLuminance = 1000 * LUMINANCE_SCALE,
                .minDisplayMasteringLuminance = 50,
                .maxContentLightLevel = 1000,
                .maxPicAverageLightLevel = 400,
        };
        populate_hdr_info_vui(&info, &session->config.stream);
        SS4S_PlayerVideoSetHDRInfo(session->player, &info);
    }
}

void streaming_error(session_t *session, int code, const char *fmt, ...) {
    SDL_LockMutex(session->state_lock);
    streaming_errno = code;
    va_list arglist;
    va_start(arglist, fmt);
    vsnprintf(streaming_errmsg, sizeof(streaming_errmsg) / sizeof(char), fmt, arglist);
    va_end(arglist);
    SDL_UnlockMutex(session->state_lock);
}

bool streaming_sops_supported(PDISPLAY_MODE modes, int w, int h, int fps) {
    for (PDISPLAY_MODE cur = modes; cur != NULL; cur = cur->next) {
        if (cur->width == w && cur->height == h && cur->refresh == fps) {
            return true;
        }
    }
    return false;
}

void session_config_init(app_t *app, session_config_t *config, const SERVER_DATA *server,
                         const CONFIGURATION *app_config) {
    config->stream = app_config->stream;
    config->vmouse = app_config->virtual_mouse;
    config->hardware_mouse = app_config->hardware_mouse;
    config->local_audio = app_config->localaudio;
    config->view_only = app_config->viewonly;
    config->sops = app_config->sops;
    if (app_config->stick_deadzone < 0) {
        config->stick_deadzone = 0;
    } else if (app_config->stick_deadzone > 100) {
        config->stick_deadzone = 100;
    } else {
        config->stick_deadzone = (uint8_t) app_config->stick_deadzone;
    }

    SS4S_VideoCapabilities video_cap = app->ss4s.video_cap;
    SS4S_AudioCapabilities audio_cap = app->ss4s.audio_cap;

    if (config->stream.bitrate < 0) {
        config->stream.bitrate = settings_optimal_bitrate(&video_cap, config->stream.width, config->stream.height,
                                                          config->stream.fps);
    }
    // Cap framerate to platform request
    if (video_cap.maxBitrate && config->stream.bitrate > video_cap.maxBitrate) {
        config->stream.bitrate = (int) video_cap.maxBitrate;
    }
    if (video_cap.codecs & SS4S_VIDEO_H264) {
        config->stream.supportedVideoFormats |= VIDEO_FORMAT_H264;
    }
    if (app_config->hevc && video_cap.codecs & SS4S_VIDEO_H265) {
        config->stream.supportedVideoFormats |= VIDEO_FORMAT_H265;
        if (app_config->hdr && video_cap.hdr) {
            config->stream.supportedVideoFormats |= VIDEO_FORMAT_H265_MAIN10;
        }
    }
    if (app_config->av1 && video_cap.codecs & SS4S_VIDEO_AV1) {
        config->stream.supportedVideoFormats |= VIDEO_FORMAT_AV1_MAIN8;
        if (app_config->hdr && video_cap.hdr) {
            config->stream.supportedVideoFormats |= VIDEO_FORMAT_AV1_MAIN10;
        }
    }
    // If no video format is supported, default to H.264
    if (config->stream.supportedVideoFormats == 0) {
        config->stream.supportedVideoFormats = VIDEO_FORMAT_H264;
    }
    if (video_cap.colorSpace & SS4S_VIDEO_CAP_COLORSPACE_BT2020 &&
        (config->stream.supportedVideoFormats & ~VIDEO_FORMAT_MASK_H264)) {
        config->stream.colorSpace = COLORSPACE_REC_2020;
    } else if (video_cap.colorSpace & SS4S_VIDEO_CAP_COLORSPACE_BT709) {
        config->stream.colorSpace = COLORSPACE_REC_709;
    } else {
        config->stream.colorSpace = COLORSPACE_REC_601;
    }
    config->stream.colorRange = video_cap.fullColorRange ? COLOR_RANGE_FULL : COLOR_RANGE_LIMITED;
#if FEATURE_SURROUND_SOUND
    if (audio_cap.maxChannels < CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(config->stream.audioConfiguration)) {
        switch (audio_cap.maxChannels) {
            case 2:
                config->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
                break;
            case 6:
                config->stream.audioConfiguration = AUDIO_CONFIGURATION_51_SURROUND;
                break;
            case 8:
                config->stream.audioConfiguration = AUDIO_CONFIGURATION_71_SURROUND;
                break;
        }
    }
    if (!config->stream.audioConfiguration) {
        config->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    }
#endif
    config->stream.encryptionFlags = ENCFLG_AUDIO;
}

/**
 * Populate HDR info from stream configuration.
 * Corresponds to @p avcodec_colorspace_from_sunshine_colorspace in video_colorspace.cpp in Sunshine.
 *
 * @param info Info for SS4S_PlayerVideoSetHDRInfo
 * @param config Moonlight stream configuration
 */
static void populate_hdr_info_vui(SS4S_VideoHDRInfo *info, const STREAM_CONFIGURATION *config) {
    switch (config->colorSpace) {
        case COLORSPACE_REC_601:
            info->colorPrimaries = 6 /* SMPTE 170M */;
            info->transferCharacteristics = 6 /* SMPTE 170M */;
            info->matrixCoefficients = 6 /* SMPTE 170M */;
            break;
        case COLORSPACE_REC_709:
            info->colorPrimaries = 1 /* BT.709 */;
            info->transferCharacteristics = 1 /* BT.709 */;
            info->matrixCoefficients = 1 /* BT.709 */;
            break;
        case COLORSPACE_REC_2020: {
            info->colorPrimaries = 9 /* BT.2020 */;
            info->transferCharacteristics = 16 /* SMPTE ST 2084 */;
            info->matrixCoefficients = 9 /* BT.2020 NCL */;
            break;
        }
    }
    info->videoFullRange = config->colorRange == COLOR_RANGE_FULL;
}

