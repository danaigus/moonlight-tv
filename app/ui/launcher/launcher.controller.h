#pragma once

#include <stdbool.h>

#include "ui/config.h"
#include "lvgl/lv_obj_controller.h"

#include "lvgl.h"

#include "backend/pcmanager.h"

typedef struct {
    lv_obj_controller_t base;
    PCMANAGER_CALLBACKS _pcmanager_callbacks;
    lv_obj_t *nav;
    lv_obj_t *detail;
    lv_obj_t *pclist;
    PSERVER_LIST selected_server;
    lv_controller_manager_t *pane_manager;
    lv_style_transition_dsc_t tr_nav;
    lv_style_transition_dsc_t tr_detail;
} launcher_controller_t;


lv_obj_t *launcher_win_create(lv_obj_controller_t *self, lv_obj_t *parent);

extern const lv_obj_controller_class_t launcher_controller_class;