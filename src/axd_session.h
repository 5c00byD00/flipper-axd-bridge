#pragma once

#include "axd_app.h"

typedef struct AxdApp AxdApp;

AxdApp* axd_app_alloc(void);
void axd_app_free(AxdApp* app);
void axd_app_post(AxdApp* app, uint8_t id);
void axd_app_lock(AxdApp* app);
void axd_app_unlock(AxdApp* app);
AxdModel* axd_app_model(AxdApp* app);
