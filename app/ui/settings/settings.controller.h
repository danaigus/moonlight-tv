#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui/config.h"
#include "lvgl/lv_obj_controller.h"
#include "util/navkey.h"

#include "lvgl.h"

#include "stream/settings.h"
#include "app.h"

typedef struct {
    lv_obj_controller_t base;
    lv_controller_manager_t *pane_manager;
    lv_obj_t *nav, *detail;
} settings_controller_t;

lv_obj_t *settings_win_create(struct lv_obj_controller_t *self, lv_obj_t *parent);

extern const lv_obj_controller_class_t settings_controller_cls;