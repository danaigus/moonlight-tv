#include "stream/session.h"
#include "stream/input/absinput.h"
#include "util/logging.h"

static void connection_terminated(int errorCode) {
    applog_e("Session", "Connection terminated, errorCode = 0x%x", errorCode);
    streaming_error(0, "Connection terminated, errorCode = 0x%x", errorCode);
    streaming_interrupt(false);
}

static void connection_log_message(const char *format, ...) {
    va_list arglist;
    va_start(arglist, format);
    app_logvprintf("INFO", "Limelight", format, arglist);
    va_end(arglist);
}

static void connection_status_update(int status) {
    switch (status) {
        case CONN_STATUS_OKAY:
            applog_i("Session", "Connection is okay");
            break;
        case CONN_STATUS_POOR:
            applog_w("Session", "Connection is poor");
            break;
    }
}

static void connection_stage_failed(int stage, int errorCode) {
    const char *stageName = LiGetStageName(stage);
    applog_e("Session", "Connection failed at %s, errorCode = %d", stageName, errorCode);
    streaming_error(0, "Connection failed at %s, errorCode = %d", stageName, errorCode);
}

CONNECTION_LISTENER_CALLBACKS connection_callbacks = {
        .stageStarting = NULL,
        .stageComplete = NULL,
        .stageFailed = connection_stage_failed,
        .connectionStarted = NULL,
        .connectionTerminated = connection_terminated,
        .logMessage = connection_log_message,
        .rumble = absinput_rumble,
        .connectionStatusUpdate = connection_status_update,
};
