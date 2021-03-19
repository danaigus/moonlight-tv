/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include <Limelight.h>

#include <dlfcn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#include "video/presenter.h"

#define IS_EMBEDDED(SYSTEM) SYSTEM != SDL

enum platform
{
    NONE = 0,
    SDL,
    X11,
    X11_VDPAU,
    X11_VAAPI,
    PI,
    MMAL,
    IMX,
    AML,
    RK,
    NDL,
    LGNC,
    SMP,
    FAKE
};

extern enum platform platform_current;

enum platform platform_init(const char *name, int argc, char *argv[]);
PDECODER_RENDERER_CALLBACKS platform_get_video(enum platform system);
PAUDIO_RENDERER_CALLBACKS platform_get_audio(enum platform system, char *audio_device);
PVIDEO_PRESENTER_CALLBACKS platform_get_presenter(enum platform system);

bool platform_supports_hevc(enum platform system);
const char *platform_name(enum platform system);

void platform_start(enum platform system);
void platform_stop(enum platform system);

void platform_finalize(enum platform system);

bool platform_is_software(enum platform system);