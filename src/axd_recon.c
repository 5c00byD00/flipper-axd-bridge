#include "axd_session.h"

#include <furi.h>
#include <stdio.h>
#include <string.h>
#include <gui/gui.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>

typedef enum {
    ViewMain,
    ViewRecon,
    ViewAp,
    ViewClient,
    ViewWardrive,
    ViewWdStatus,
    ViewLog,
    ViewFleet,
    ViewFleetStatus,
    ViewSerial,
} AxdViewId;

typedef struct {
    AxdApp* bridge;
    Gui* gui;
    ViewDispatcher* vd;
    Submenu* main_menu;
    Submenu* recon_menu;
    Submenu* wardrive_menu;
    Submenu* fleet_menu;
    View* ap_view;
    View* client_view;
    View* wd_view;
    View* fleet_view;
    TextBox* log_box;
    TextBox* serial_box;
    char serial_shadow[1200];
    char log_shadow[1500];
    uint32_t current;
    uint32_t nav[8];
    uint8_t nav_depth;
} AxdGui;

static void fmt_fixed(char* out, size_t n, double value, int digits) {
    int neg = value < 0;
    if(neg) value = -value;
    int scale = 1;
    for(int i = 0; i < digits; i++) scale *= 10;
    int whole = (int)value;
    int frac = (int)((value - (double)whole) * (double)scale + ((double)1 / (double)2));
    if(frac >= scale) {
        frac -= scale;
        whole++;
    }
    snprintf(out, n, "%s%d.%0*d", neg ? "-" : "", whole, digits, frac);
}

static void show(AxdGui* gui, uint32_t next) {
    if(gui->nav_depth < COUNT_OF(gui->nav)) gui->nav[gui->nav_depth++] = gui->current;
    gui->current = next;
    view_dispatcher_switch_to_view(gui->vd, next);
}

static bool on_back(void* context) {
    AxdGui* gui = context;
    if(gui->nav_depth == 0) return false;
    gui->current = gui->nav[--gui->nav_depth];
    view_dispatcher_switch_to_view(gui->vd, gui->current);
    return true;
}

static void bump(View* view) {
    with_view_model(view, uint32_t * frame, { (*frame)++; }, true);
}

static void on_tick(void* context) {
    AxdGui* gui = context;
    bump(gui->ap_view);
    bump(gui->client_view);
    bump(gui->wd_view);
    bump(gui->fleet_view);
    axd_app_lock(gui->bridge);
    AxdModel* model = axd_app_model(gui->bridge);
    bool serial_dirty = gui->current == ViewSerial && model->serial_dirty;
    bool log_dirty = gui->current == ViewLog && model->log_dirty;
    if(serial_dirty) {
        snprintf(
            gui->serial_shadow,
            sizeof(gui->serial_shadow),
            "%s",
            model->serial_text[0] ? model->serial_text : "No data yet");
        model->serial_dirty = false;
    }
    if(log_dirty) {
        snprintf(
            gui->log_shadow,
            sizeof(gui->log_shadow),
            "%s",
            model->log_text[0] ? model->log_text : "No log yet");
        model->log_dirty = false;
    }
    axd_app_unlock(gui->bridge);
    if(serial_dirty) {
        text_box_set_text(gui->serial_box, gui->serial_shadow);
        text_box_set_focus(gui->serial_box, TextBoxFocusEnd);
    }
    if(log_dirty) {
        text_box_set_text(gui->log_box, gui->log_shadow);
        text_box_set_focus(gui->log_box, TextBoxFocusEnd);
    }
}

static void on_main(void* context, uint32_t index) {
    AxdGui* gui = context;
    if(index == 0) show(gui, ViewRecon);
    else if(index == 1) show(gui, ViewWardrive);
    else if(index == 2) show(gui, ViewFleet);
    else if(index == 3) show(gui, ViewSerial);
}

static void on_recon(void* context, uint32_t index) {
    AxdGui* gui = context;
    switch(index) {
    case 0:
        show(gui, ViewAp);
        break;
    case 1:
        axd_app_post(gui->bridge, AxdMsgRefresh);
        show(gui, ViewAp);
        break;
    case 2:
        axd_app_post(gui->bridge, AxdMsgStopScan);
        break;
    case 3:
        show(gui, ViewClient);
        break;
    case 4:
        axd_app_post(gui->bridge, AxdMsgClientsStart);
        show(gui, ViewClient);
        break;
    case 5:
        axd_app_post(gui->bridge, AxdMsgClientsStop);
        break;
    default:
        break;
    }
}

static void on_wardrive(void* context, uint32_t index) {
    AxdGui* gui = context;
    switch(index) {
    case 0:
        show(gui, ViewWdStatus);
        break;
    case 1:
        axd_app_post(gui->bridge, AxdMsgApLogStart);
        show(gui, ViewWdStatus);
        break;
    case 2:
        axd_app_post(gui->bridge, AxdMsgApLogStop);
        break;
    case 3:
        axd_app_post(gui->bridge, AxdMsgBridgeStart);
        show(gui, ViewWdStatus);
        break;
    case 4:
        axd_app_post(gui->bridge, AxdMsgBridgeStop);
        break;
    case 5:
        axd_app_post(gui->bridge, AxdMsgNmea);
        break;
    case 6:
        axd_app_post(gui->bridge, AxdMsgLoadLog);
        show(gui, ViewLog);
        break;
    case 7:
        axd_app_post(gui->bridge, AxdMsgExport);
        break;
    default:
        break;
    }
}

static void on_fleet(void* context, uint32_t index) {
    AxdGui* gui = context;
    switch(index) {
    case 0:
        show(gui, ViewFleetStatus);
        break;
    case 1:
        axd_app_post(gui->bridge, AxdMsgFleetStart);
        show(gui, ViewFleetStatus);
        break;
    case 2:
        axd_app_post(gui->bridge, AxdMsgFleetJoin);
        show(gui, ViewFleetStatus);
        break;
    case 3:
        axd_app_post(gui->bridge, AxdMsgFleetLeave);
        break;
    default:
        break;
    }
}

static void draw_ap(Canvas* canvas, void* context) {
    AxdGui* gui = context;
    axd_app_lock(gui->bridge);
    AxdModel* model = axd_app_model(gui->bridge);
    int count = model->ap_count;
    int cursor = model->ap_cursor;
    if(cursor >= count) cursor = count > 0 ? count - 1 : 0;
    AxdAp ap;
    memset(&ap, 0, sizeof(ap));
    if(count > 0) ap = model->aps[cursor];
    char status[72];
    snprintf(status, sizeof(status), "%s", model->status);
    bool scanning = model->scanning;
    axd_app_unlock(gui->bridge);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    char head[32];
    if(count > 0) snprintf(head, sizeof(head), "AP %d/%d%s", cursor + 1, count, scanning ? " ..." : "");
    else snprintf(head, sizeof(head), "%s", scanning ? "Scanning..." : "No access points");
    canvas_draw_str(canvas, 0, 8, head);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 20, count ? (ap.ssid[0] ? ap.ssid : "<hidden>") : "OK to scan");
    canvas_set_font(canvas, FontSecondary);
    if(count) {
        canvas_draw_str(canvas, 0, 31, ap.bssid);
        char radio[32];
        snprintf(radio, sizeof(radio), "%d dBm  ch %d", ap.rssi, ap.channel);
        canvas_draw_str(canvas, 0, 40, radio);
        canvas_draw_str(canvas, 0, 49, ap.auth[0] ? ap.auth : "enc: not on UART");
    } else {
        canvas_draw_str(canvas, 0, 36, status);
    }
    canvas_draw_str(canvas, 0, 62, "OK scan   Left stop");
}

static bool input_ap(InputEvent* event, void* context) {
    AxdGui* gui = context;
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        axd_app_post(gui->bridge, AxdMsgRefresh);
        return true;
    }
    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        axd_app_post(gui->bridge, AxdMsgStopScan);
        return true;
    }
    if((event->type == InputTypeShort || event->type == InputTypeRepeat) &&
       (event->key == InputKeyUp || event->key == InputKeyDown)) {
        axd_app_lock(gui->bridge);
        AxdModel* model = axd_app_model(gui->bridge);
        if(model->ap_count > 0) {
            if(event->key == InputKeyUp) {
                if(model->ap_cursor > 0) model->ap_cursor--;
            } else if(model->ap_cursor + 1 < model->ap_count) {
                model->ap_cursor++;
            }
        }
        axd_app_unlock(gui->bridge);
        return true;
    }
    return false;
}

static void draw_client(Canvas* canvas, void* context) {
    AxdGui* gui = context;
    axd_app_lock(gui->bridge);
    AxdModel* model = axd_app_model(gui->bridge);
    bool on = model->clients_on;
    bool known = model->clients_count_valid;
    int count = model->client_count;
    char status[72];
    snprintf(status, sizeof(status), "%s", model->status);
    axd_app_unlock(gui->bridge);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 12, on ? "Listening" : "Clients");
    canvas_set_font(canvas, FontSecondary);
    if(known) {
        char line[32];
        snprintf(line, sizeof(line), "Last stop: %d", count);
        canvas_draw_str(canvas, 0, 26, line);
    } else {
        canvas_draw_str(canvas, 0, 26, on ? "Waiting for summary" : "Not started");
    }
    canvas_draw_str(canvas, 0, 40, "UART reports the count");
    canvas_draw_str(canvas, 0, 50, status);
    canvas_draw_str(canvas, 0, 62, "OK start  Left stop");
}

static bool input_client(InputEvent* event, void* context) {
    AxdGui* gui = context;
    if(event->type != InputTypeShort) return false;
    if(event->key == InputKeyOk) {
        axd_app_post(gui->bridge, AxdMsgClientsStart);
        return true;
    }
    if(event->key == InputKeyLeft) {
        axd_app_post(gui->bridge, AxdMsgClientsStop);
        return true;
    }
    return false;
}

static void draw_wd(Canvas* canvas, void* context) {
    AxdGui* gui = context;
    axd_app_lock(gui->bridge);
    AxdModel* model = axd_app_model(gui->bridge);
    bool ap_log = model->ap_log;
    bool bridge = model->bridge_wd;
    bool gps = model->gps_valid;
    bool nmea = model->nmea_on;
    uint32_t rows = model->logged_rows;
    uint32_t wifi = model->wd_wifi;
    uint32_t ble = model->wd_ble;
    uint32_t seconds = model->wd_seconds;
    unsigned fix = model->wd_fix_pct;
    int sats = gps ? model->sats : model->wd_sats;
    double lat = model->lat;
    double lon = model->lon;
    char status[72];
    snprintf(status, sizeof(status), "%s", model->status);
    axd_app_unlock(gui->bridge);

    char lat_s[16] = "--";
    char lon_s[16] = "--";
    if(gps) {
        fmt_fixed(lat_s, sizeof(lat_s), lat, 5);
        fmt_fixed(lon_s, sizeof(lon_s), lon, 5);
    }
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    char line[40];
    snprintf(line, sizeof(line), "Log %s  Bridge %s", ap_log ? "ON" : "off", bridge ? "ON" : "off");
    canvas_draw_str(canvas, 0, 8, line);
    snprintf(line, sizeof(line), "Rows %lu  NMEA %s", (unsigned long)rows, nmea ? "on" : "off");
    canvas_draw_str(canvas, 0, 17, line);
    snprintf(line, sizeof(line), "WiFi %lu BLE %lu", (unsigned long)wifi, (unsigned long)ble);
    canvas_draw_str(canvas, 0, 26, line);
    snprintf(line, sizeof(line), "%lus  fix %u%%  sat %d", (unsigned long)seconds, fix, sats);
    canvas_draw_str(canvas, 0, 35, line);
    snprintf(line, sizeof(line), "%s", lat_s);
    canvas_draw_str(canvas, 0, 44, line);
    snprintf(line, sizeof(line), "%s", lon_s);
    canvas_draw_str(canvas, 0, 53, line);
    canvas_draw_str(canvas, 0, 62, status);
}

static void draw_fleet(Canvas* canvas, void* context) {
    AxdGui* gui = context;
    axd_app_lock(gui->bridge);
    AxdModel* model = axd_app_model(gui->bridge);
    bool listening = model->fleet_listening;
    bool active = model->fleet_active;
    bool coord = model->fleet_coordinator;
    bool ble = model->fleet_ble_node;
    unsigned code = model->fleet_code;
    uint32_t session = model->fleet_session;
    int members = model->fleet_members;
    int index = model->fleet_index;
    char role[8];
    snprintf(role, sizeof(role), "%s", model->fleet_role);
    uint32_t wifi = model->fleet_wifi ? model->fleet_wifi : model->wd_wifi;
    uint32_t ble_n = model->fleet_ble ? model->fleet_ble : model->wd_ble;
    char detail[64];
    snprintf(detail, sizeof(detail), "%s", model->fleet_detail);
    char status[72];
    snprintf(status, sizeof(status), "%s", model->status);
    axd_app_unlock(gui->bridge);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    const char* title = "Idle";
    if(listening) title = "Listening";
    else if(active && coord) title = "Coordinator";
    else if(active) title = "Member";
    canvas_draw_str(canvas, 0, 12, title);
    canvas_set_font(canvas, FontSecondary);
    char line[40];
    snprintf(line, sizeof(line), "Code %04u  N %d", code, members);
    canvas_draw_str(canvas, 0, 24, line);
    snprintf(line, sizeof(line), "Idx %d %s%s", index, role[0] ? role : "-", ble ? " BLE" : "");
    canvas_draw_str(canvas, 0, 33, line);
    snprintf(line, sizeof(line), "Sess %lu", (unsigned long)session);
    canvas_draw_str(canvas, 0, 42, line);
    snprintf(line, sizeof(line), "WiFi %lu BLE %lu", (unsigned long)wifi, (unsigned long)ble_n);
    canvas_draw_str(canvas, 0, 51, line);
    canvas_draw_str(canvas, 0, 62, status[0] ? status : detail);
}

static View* make_view(AxdGui* gui, ViewDrawCallback draw, ViewInputCallback input) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLockFree, sizeof(uint32_t));
    view_set_context(view, gui);
    view_set_draw_callback(view, draw);
    view_set_input_callback(view, input);
    return view;
}

static bool input_ignore(InputEvent* event, void* context) {
    UNUSED(event);
    UNUSED(context);
    return false;
}

int32_t axd_recon_app(void* p) {
    UNUSED(p);
    AxdGui* gui = malloc(sizeof(AxdGui));
    memset(gui, 0, sizeof(AxdGui));
    gui->bridge = axd_app_alloc();
    gui->gui = furi_record_open(RECORD_GUI);
    gui->vd = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(gui->vd, gui->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(gui->vd, gui);
    view_dispatcher_set_navigation_event_callback(gui->vd, on_back);
    view_dispatcher_set_tick_event_callback(gui->vd, on_tick, 200);

    gui->main_menu = submenu_alloc();
    submenu_set_header(gui->main_menu, "AxD Recon");
    submenu_add_item(gui->main_menu, "Recon", 0, on_main, gui);
    submenu_add_item(gui->main_menu, "Wardrive", 1, on_main, gui);
    submenu_add_item(gui->main_menu, "Fleet", 2, on_main, gui);
    submenu_add_item(gui->main_menu, "Serial log", 3, on_main, gui);

    gui->recon_menu = submenu_alloc();
    submenu_set_header(gui->recon_menu, "Recon");
    submenu_add_item(gui->recon_menu, "Access points", 0, on_recon, gui);
    submenu_add_item(gui->recon_menu, "Refresh scan", 1, on_recon, gui);
    submenu_add_item(gui->recon_menu, "Stop / Home", 2, on_recon, gui);
    submenu_add_item(gui->recon_menu, "Clients", 3, on_recon, gui);
    submenu_add_item(gui->recon_menu, "Start clients", 4, on_recon, gui);
    submenu_add_item(gui->recon_menu, "Stop clients", 5, on_recon, gui);

    gui->wardrive_menu = submenu_alloc();
    submenu_set_header(gui->wardrive_menu, "Wardrive");
    submenu_add_item(gui->wardrive_menu, "Status", 0, on_wardrive, gui);
    submenu_add_item(gui->wardrive_menu, "Start AP log", 1, on_wardrive, gui);
    submenu_add_item(gui->wardrive_menu, "Stop AP log", 2, on_wardrive, gui);
    submenu_add_item(gui->wardrive_menu, "Start bridge drive", 3, on_wardrive, gui);
    submenu_add_item(gui->wardrive_menu, "Stop bridge drive", 4, on_wardrive, gui);
    submenu_add_item(gui->wardrive_menu, "Toggle GPS NMEA", 5, on_wardrive, gui);
    submenu_add_item(gui->wardrive_menu, "View log", 6, on_wardrive, gui);
    submenu_add_item(gui->wardrive_menu, "Export log", 7, on_wardrive, gui);

    gui->fleet_menu = submenu_alloc();
    submenu_set_header(gui->fleet_menu, "Fleet");
    submenu_add_item(gui->fleet_menu, "Status", 0, on_fleet, gui);
    submenu_add_item(gui->fleet_menu, "Start fleet", 1, on_fleet, gui);
    submenu_add_item(gui->fleet_menu, "Join fleet", 2, on_fleet, gui);
    submenu_add_item(gui->fleet_menu, "Leave fleet", 3, on_fleet, gui);

    gui->ap_view = make_view(gui, draw_ap, input_ap);
    gui->client_view = make_view(gui, draw_client, input_client);
    gui->wd_view = make_view(gui, draw_wd, input_ignore);
    gui->fleet_view = make_view(gui, draw_fleet, input_ignore);

    gui->log_box = text_box_alloc();
    text_box_set_font(gui->log_box, TextBoxFontText);
    text_box_set_text(gui->log_box, "No log yet");
    gui->serial_box = text_box_alloc();
    text_box_set_font(gui->serial_box, TextBoxFontText);
    text_box_set_text(gui->serial_box, "Waiting for bridge");

    view_dispatcher_add_view(gui->vd, ViewMain, submenu_get_view(gui->main_menu));
    view_dispatcher_add_view(gui->vd, ViewRecon, submenu_get_view(gui->recon_menu));
    view_dispatcher_add_view(gui->vd, ViewWardrive, submenu_get_view(gui->wardrive_menu));
    view_dispatcher_add_view(gui->vd, ViewFleet, submenu_get_view(gui->fleet_menu));
    view_dispatcher_add_view(gui->vd, ViewAp, gui->ap_view);
    view_dispatcher_add_view(gui->vd, ViewClient, gui->client_view);
    view_dispatcher_add_view(gui->vd, ViewWdStatus, gui->wd_view);
    view_dispatcher_add_view(gui->vd, ViewFleetStatus, gui->fleet_view);
    view_dispatcher_add_view(gui->vd, ViewLog, text_box_get_view(gui->log_box));
    view_dispatcher_add_view(gui->vd, ViewSerial, text_box_get_view(gui->serial_box));

    gui->current = ViewMain;
    view_dispatcher_switch_to_view(gui->vd, ViewMain);
    view_dispatcher_run(gui->vd);

    view_dispatcher_remove_view(gui->vd, ViewMain);
    view_dispatcher_remove_view(gui->vd, ViewRecon);
    view_dispatcher_remove_view(gui->vd, ViewWardrive);
    view_dispatcher_remove_view(gui->vd, ViewFleet);
    view_dispatcher_remove_view(gui->vd, ViewAp);
    view_dispatcher_remove_view(gui->vd, ViewClient);
    view_dispatcher_remove_view(gui->vd, ViewWdStatus);
    view_dispatcher_remove_view(gui->vd, ViewFleetStatus);
    view_dispatcher_remove_view(gui->vd, ViewLog);
    view_dispatcher_remove_view(gui->vd, ViewSerial);
    view_dispatcher_free(gui->vd);
    submenu_free(gui->main_menu);
    submenu_free(gui->recon_menu);
    submenu_free(gui->wardrive_menu);
    submenu_free(gui->fleet_menu);
    view_free(gui->ap_view);
    view_free(gui->client_view);
    view_free(gui->wd_view);
    view_free(gui->fleet_view);
    text_box_free(gui->log_box);
    text_box_free(gui->serial_box);
    furi_record_close(RECORD_GUI);
    axd_app_free(gui->bridge);
    free(gui);
    return 0;
}
