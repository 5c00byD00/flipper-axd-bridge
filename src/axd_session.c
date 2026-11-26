#include "axd_session.h"

#include "axd_protocol.h"

#include <expansion/expansion.h>
#include <furi.h>
#include <furi_hal_rtc.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <storage/storage.h>

#include <stdio.h>
#include <string.h>

#define AXD_LINE_MAX 320
#define AXD_RX_STREAM 8192

typedef struct {
    uint8_t id;
} AxdMsg;

struct AxdApp {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    FuriThread* worker;
    FuriStreamBuffer* rx_stream;
    FuriHalSerialHandle* serial;
    Expansion* expansion;
    Storage* storage;
    File* log_file;
    volatile bool exit_req;
    AxdModel model;

    char line[AXD_LINE_MAX];
    size_t line_len;
    bool line_overflow;

    bool scan_active;
    bool scan_cancel;
    bool ap_log_want;
    int scan_retries;
    bool saw_passive;
    uint32_t scan_t0;
    uint32_t last_ap_ms;
    bool in_gap;
    uint32_t gap_until;
    uint32_t nmea_shown_ms;
    bool saw_gga;
};

static void status_unlocked(AxdApp* app, const char* text) {
    snprintf(app->model.status, sizeof(app->model.status), "%s", text);
}

static void set_status(AxdApp* app, const char* text) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    status_unlocked(app, text);
    furi_mutex_release(app->mutex);
}

static void rebuild_serial(AxdModel* model) {
    size_t used = 0;
    model->serial_text[0] = 0;
    if(model->serial_count <= 0) return;
    int start = (model->serial_head - model->serial_count + AXD_SERIAL_LINES) % AXD_SERIAL_LINES;
    for(int i = 0; i < model->serial_count; i++) {
        const char* row = model->serial_lines[(start + i) % AXD_SERIAL_LINES];
        int wrote = snprintf(
            model->serial_text + used,
            sizeof(model->serial_text) - used,
            "%s%s",
            i ? "\n" : "",
            row);
        if(wrote < 0 || (size_t)wrote >= sizeof(model->serial_text) - used) break;
        used += (size_t)wrote;
    }
    model->serial_dirty = true;
}

static void push_serial(AxdApp* app, const char* line) {
    if(!line || !line[0] || strncmp(line, "Commands:", 9) == 0) return;
    char row[AXD_SERIAL_COLS + 1];
    size_t n = 0;
    for(const char* p = line; *p && n < AXD_SERIAL_COLS; p++) {
        unsigned char c = (unsigned char)*p;
        if(c >= 32 && c < 127) row[n++] = (char)c;
    }
    row[n] = 0;
    if(!n) return;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    snprintf(app->model.serial_lines[app->model.serial_head], AXD_SERIAL_COLS + 1, "%s", row);
    app->model.serial_head = (app->model.serial_head + 1) % AXD_SERIAL_LINES;
    if(app->model.serial_count < AXD_SERIAL_LINES) app->model.serial_count++;
    rebuild_serial(&app->model);
    furi_mutex_release(app->mutex);
}

static void tx_bytes(AxdApp* app, const char* data) {
    if(!app->serial || !data || !data[0]) return;
    furi_hal_serial_tx(app->serial, (const uint8_t*)data, strlen(data));
    furi_hal_serial_tx_wait_complete(app->serial);
}

static bool tx_op(AxdApp* app, unsigned op) {
    char buf[16];
    if(!axd_format_command(buf, sizeof(buf), op)) return false;
    tx_bytes(app, buf);
    return true;
}

static void tx_scan_burst(AxdApp* app) {
    // Opcode 1 clears wifiCount and does not print APs. The following stop
    // lands before that async scan can merge, so opcode 3 (channel map) takes
    // the empty-list path and scanWifi() prints one "[wifi] rssi dBm chN BSSID SSID"
    // line per AP. Verified in input.ino / link.ino / AWOKxDAG.ino.
    tx_op(app, AXD_OP_STOP);
    tx_op(app, AXD_OP_WIFI_SCAN);
    tx_op(app, AXD_OP_STOP);
    tx_op(app, AXD_OP_CHANNEL_MAP);
}

static void fmt_coord(char* out, size_t n, double value) {
    int neg = value < 0;
    if(neg) value = -value;
    int whole = (int)value;
    int frac = (int)((value - (double)whole) * (double)1000000 + ((double)1 / (double)2));
    if(frac >= 1000000) {
        frac -= 1000000;
        whole++;
    }
    snprintf(out, n, "%s%d.%06d", neg ? "-" : "", whole, frac);
}

static void rtc_text(char* out, size_t n) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    snprintf(
        out,
        n,
        "%04u-%02u-%02u %02u:%02u:%02u",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second);
}

static void csv_quote(char* dst, size_t n, const char* src) {
    size_t w = 0;
    if(n == 0) return;
    if(w + 1 < n) dst[w++] = '"';
    if(!src) src = "";
    for(; *src && w + 3 < n; src++) {
        if(*src == '"') dst[w++] = '"';
        dst[w++] = *src;
    }
    if(w + 1 < n) dst[w++] = '"';
    dst[w] = 0;
}

static void log_row(
    AxdApp* app,
    const char* kind,
    const char* ssid,
    const char* bssid,
    int channel,
    int rssi,
    const char* auth,
    bool gps,
    double lat,
    double lon,
    double alt,
    int sats,
    uint32_t wifi,
    uint32_t ble,
    const char* note) {
    if(!app->log_file) return;
    char rtc[32];
    char qkind[24];
    char qssid[80];
    char qbssid[40];
    char qauth[96];
    char qnote[80];
    char lat_s[20];
    char lon_s[20];
    char alt_s[20];
    rtc_text(rtc, sizeof(rtc));
    csv_quote(qkind, sizeof(qkind), kind);
    csv_quote(qssid, sizeof(qssid), ssid ? ssid : "");
    csv_quote(qbssid, sizeof(qbssid), bssid ? bssid : "");
    csv_quote(qauth, sizeof(qauth), auth ? auth : "");
    csv_quote(qnote, sizeof(qnote), note ? note : "");
    lat_s[0] = lon_s[0] = alt_s[0] = 0;
    if(gps) {
        fmt_coord(lat_s, sizeof(lat_s), lat);
        fmt_coord(lon_s, sizeof(lon_s), lon);
        fmt_coord(alt_s, sizeof(alt_s), alt);
    }
    char row[420];
    int wrote = snprintf(
        row,
        sizeof(row),
        "\"%s\",%s,%s,%s,%d,%d,%s,%s,%s,%s,%d,%lu,%lu,%s\n",
        rtc,
        qkind,
        qssid,
        qbssid,
        channel,
        rssi,
        qauth,
        lat_s,
        lon_s,
        alt_s,
        sats,
        (unsigned long)wifi,
        (unsigned long)ble,
        qnote);
    if(wrote > 0 && (size_t)wrote < sizeof(row)) {
        storage_file_write(app->log_file, row, (uint16_t)wrote);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->model.logged_rows++;
        furi_mutex_release(app->mutex);
    }
}

static void close_log(AxdApp* app) {
    if(!app->log_file) return;
    storage_file_close(app->log_file);
    storage_file_free(app->log_file);
    app->log_file = NULL;
}

static bool open_log(AxdApp* app) {
    close_log(app);
    storage_common_mkdir(app->storage, "/ext/apps_data");
    storage_common_mkdir(app->storage, "/ext/apps_data/axd_recon");
    storage_common_mkdir(app->storage, "/ext/apps_data/axd_recon/export");
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    char path[96];
    snprintf(
        path,
        sizeof(path),
        "/ext/apps_data/axd_recon/wd_%04u%02u%02u_%02u%02u%02u.csv",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second);
    File* file = storage_file_alloc(app->storage);
    if(!storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(file);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->model.sd_ok = false;
        status_unlocked(app, "SD write failed");
        furi_mutex_release(app->mutex);
        return false;
    }
    const char* header =
        "rtc,kind,ssid,bssid,channel,rssi,auth,lat,lon,alt,sats,wifi_count,ble_count,note\n";
    storage_file_write(file, header, strlen(header));
    app->log_file = file;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    snprintf(app->model.log_path, sizeof(app->model.log_path), "%s", path);
    app->model.sd_ok = true;
    app->model.logged_rows = 0;
    furi_mutex_release(app->mutex);
    return true;
}

static void upsert_ap(AxdModel* model, const AxdEvent* ev) {
    for(int i = 0; i < model->ap_count; i++) {
        if(strcmp(model->aps[i].bssid, ev->bssid) == 0) {
            model->aps[i].rssi = (int16_t)ev->rssi;
            model->aps[i].channel = (int16_t)ev->channel;
            if(ev->ssid[0]) snprintf(model->aps[i].ssid, sizeof(model->aps[i].ssid), "%s", ev->ssid);
            if(ev->auth[0]) snprintf(model->aps[i].auth, sizeof(model->aps[i].auth), "%s", ev->auth);
            return;
        }
    }
    if(model->ap_count >= AXD_MAX_APS) return;
    AxdAp* ap = &model->aps[model->ap_count++];
    memset(ap, 0, sizeof(*ap));
    snprintf(ap->ssid, sizeof(ap->ssid), "%s", ev->ssid);
    snprintf(ap->bssid, sizeof(ap->bssid), "%s", ev->bssid);
    snprintf(ap->auth, sizeof(ap->auth), "%s", ev->auth);
    ap->rssi = (int16_t)ev->rssi;
    ap->channel = (int16_t)ev->channel;
}

static void apply_event(AxdApp* app, const AxdEvent* ev) {
    bool gps = false;
    double lat = 0, lon = 0, alt = 0;
    int sats = 0;
    char ssid[33];
    char bssid[18];
    char auth[40];
    char note[48];
    int channel = 0;
    int rssi = 0;
    uint32_t wifi = 0;
    uint32_t ble = 0;
    bool write_ap = false;
    bool write_fix = false;
    bool write_stat = false;

    ssid[0] = bssid[0] = auth[0] = note[0] = 0;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    AxdModel* m = &app->model;
    switch(ev->type) {
    case AxdEvtWifiPassiveStarted:
        m->scanning = true;
        m->ap_count = 0;
        m->ap_cursor = 0;
        status_unlocked(app, "Scanning...");
        break;
    case AxdEvtWifiContinuousStarted:
        if(app->scan_active) status_unlocked(app, "Clearing AP list...");
        break;
    case AxdEvtWifiContinuousStopped:
        break;
    case AxdEvtWifiScanFailed:
        m->scanning = false;
        status_unlocked(app, "Scan failed");
        break;
    case AxdEvtWifiAp:
    case AxdEvtWigleRow:
        upsert_ap(m, ev);
        status_unlocked(app, ev->type == AxdEvtWigleRow ? "Wardrive row" : "AP list updated");
        if(app->log_file) {
            write_ap = true;
            snprintf(ssid, sizeof(ssid), "%s", ev->ssid);
            snprintf(bssid, sizeof(bssid), "%s", ev->bssid);
            snprintf(auth, sizeof(auth), "%s", ev->auth);
            channel = ev->channel;
            rssi = ev->rssi;
            gps = ev->gps_valid || m->gps_valid;
            lat = ev->gps_valid ? ev->lat : m->lat;
            lon = ev->gps_valid ? ev->lon : m->lon;
            alt = ev->gps_valid ? ev->alt : m->alt;
            sats = ev->gps_valid ? ev->sats : m->sats;
            if(ev->type == AxdEvtWifiAp && !ev->auth[0]) {
                snprintf(note, sizeof(note), "uart-omits-auth");
            }
        }
        break;
    case AxdEvtClientsStarted:
        m->clients_on = true;
        m->clients_count_valid = false;
        status_unlocked(app, "Client sniffer on");
        break;
    case AxdEvtClientsStopped:
        m->clients_on = false;
        m->clients_count_valid = true;
        m->client_count = ev->count;
        status_unlocked(app, "Client sniffer stopped");
        break;
    case AxdEvtWardriveStarted:
    case AxdEvtLinkStarted:
        m->bridge_wd = true;
        status_unlocked(app, "Bridge wardrive on");
        break;
    case AxdEvtWardriveStopped:
        m->bridge_wd = false;
        m->wd_wifi = ev->wifi;
        m->wd_ble = ev->ble;
        status_unlocked(app, "Bridge wardrive stopped");
        break;
    case AxdEvtLinkStopped:
        m->bridge_wd = false;
        m->wd_wifi = ev->wifi;
        status_unlocked(app, "Fleet drive stopped");
        break;
    case AxdEvtWardriveNote:
    case AxdEvtSdNote:
        snprintf(m->fleet_detail, sizeof(m->fleet_detail), "%s", ev->text);
        break;
    case AxdEvtWdStat:
        m->wd_valid = true;
        m->wd_active = ev->active;
        m->wd_seconds = ev->seconds;
        m->wd_metres = ev->metres;
        m->wd_fix_pct = ev->fix_pct;
        m->wd_wifi = ev->wifi;
        m->wd_ble = ev->ble;
        m->wd_rows = ev->rows;
        m->wd_sats = ev->sats;
        m->wd_hdop = ev->hdop;
        m->wd_speed = ev->speed_kmh;
        m->wd_storage = ev->storage;
        m->wd_fix_flag = ev->gps_fix_flag;
        snprintf(m->wd_file, sizeof(m->wd_file), "%s", ev->text);
        m->bridge_wd = ev->active != 0;
        if(app->log_file) {
            write_stat = true;
            wifi = ev->wifi;
            ble = ev->ble;
            gps = m->gps_valid;
            lat = m->lat;
            lon = m->lon;
            alt = m->alt;
            sats = m->sats;
            snprintf(note, sizeof(note), "%s", ev->text);
        }
        break;
    case AxdEvtNmea:
        if(ev->gps_valid) {
            m->gps_valid = true;
            m->lat = ev->lat;
            m->lon = ev->lon;
            if(ev->text[0] == 'G') m->alt = ev->alt;
            if(ev->sats) m->sats = ev->sats;
        } else if(ev->text[0] == 'G') {
            m->gps_valid = false;
        }
        if(app->log_file && ev->gps_valid && (ev->text[0] == 'G' || !app->saw_gga)) {
            write_fix = true;
            gps = true;
            lat = ev->lat;
            lon = ev->lon;
            alt = m->alt;
            sats = m->sats;
        }
        if(ev->text[0] == 'G') app->saw_gga = ev->gps_valid;
        break;
    case AxdEvtFleetArmed:
        m->fleet_listening = true;
        m->fleet_active = false;
        status_unlocked(app, "Listening for fleet");
        break;
    case AxdEvtFleetCoordinator:
        m->fleet_active = true;
        m->fleet_coordinator = true;
        m->fleet_listening = false;
        m->fleet_session = ev->session;
        m->fleet_code = ev->code;
        status_unlocked(app, "Fleet coordinator");
        break;
    case AxdEvtFleetJoined:
        m->fleet_active = true;
        snprintf(m->fleet_detail, sizeof(m->fleet_detail), "Member %d joined", ev->index);
        break;
    case AxdEvtFleetRoster:
        m->fleet_active = true;
        m->fleet_listening = false;
        m->fleet_members = ev->count;
        m->fleet_index = ev->index;
        m->fleet_slice = ev->slice;
        m->fleet_slice_count = ev->slice_count;
        m->fleet_coord = ev->coord;
        snprintf(m->fleet_role, sizeof(m->fleet_role), "%s", ev->role);
        m->fleet_coordinator = ev->index == ev->coord;
        status_unlocked(app, "Fleet roster");
        break;
    case AxdEvtFleetAgg:
        m->fleet_members = ev->count;
        m->fleet_wifi = ev->wifi;
        m->fleet_ble = ev->ble;
        break;
    case AxdEvtFleetTimeout:
        snprintf(m->fleet_detail, sizeof(m->fleet_detail), "Member %d timed out", ev->index);
        break;
    case AxdEvtFleetLeft:
        m->fleet_active = false;
        m->fleet_listening = false;
        m->fleet_coordinator = false;
        m->fleet_ble_node = false;
        m->fleet_members = 0;
        status_unlocked(app, "Left fleet");
        break;
    case AxdEvtFleetBle:
        m->fleet_ble_node = true;
        status_unlocked(app, "Fleet BLE node");
        break;
    case AxdEvtFleetNote:
        snprintf(m->fleet_detail, sizeof(m->fleet_detail), "%s", ev->text);
        break;
    case AxdEvtRemoteCmd:
        snprintf(m->fleet_detail, sizeof(m->fleet_detail), "op %u", ev->op);
        break;
    case AxdEvtNone:
        break;
    }
    furi_mutex_release(app->mutex);

    if(write_ap) {
        log_row(
            app,
            "ap",
            ssid,
            bssid,
            channel,
            rssi,
            auth,
            gps,
            lat,
            lon,
            alt,
            sats,
            0,
            0,
            note);
    } else if(write_fix) {
        log_row(app, "fix", "", "", 0, 0, "", true, lat, lon, alt, sats, 0, 0, "");
    } else if(write_stat) {
        log_row(app, "wdstat", "", "", 0, 0, "", gps, lat, lon, alt, sats, wifi, ble, note);
    }
}

static void finish_scan(AxdApp* app, const char* text);

static void on_line(AxdApp* app, const char* line) {
    AxdEvent ev;
    bool known = axd_parse_line(line, &ev);
    if(known && ev.type == AxdEvtNmea) {
        uint32_t now = furi_get_tick();
        if(now - app->nmea_shown_ms > 2000) {
            app->nmea_shown_ms = now;
            char shown[AXD_SERIAL_COLS + 1];
            char lat[16];
            char lon[16];
            if(ev.gps_valid) {
                fmt_coord(lat, sizeof(lat), ev.lat);
                fmt_coord(lon, sizeof(lon), ev.lon);
                snprintf(shown, sizeof(shown), "GPS %s %s", lat, lon);
            } else {
                snprintf(shown, sizeof(shown), "GPS no fix");
            }
            push_serial(app, shown);
        }
    } else {
        push_serial(app, line);
    }
    if(!known) return;
    if(ev.type == AxdEvtWifiPassiveStarted) {
        app->saw_passive = true;
        app->last_ap_ms = 0;
    } else if(ev.type == AxdEvtWifiAp || ev.type == AxdEvtWigleRow) {
        app->last_ap_ms = furi_get_tick();
    }
    apply_event(app, &ev);
    if(ev.type == AxdEvtWifiScanFailed && app->scan_active) finish_scan(app, "Scan failed");
}

static void feed_byte(AxdApp* app, uint8_t byte) {
    if(byte == '\n') {
        if(!app->line_overflow && app->line_len > 0) {
            app->line[app->line_len] = 0;
            on_line(app, app->line);
        }
        app->line_len = 0;
        app->line_overflow = false;
        return;
    }
    if(byte == '\r') return;
    if(app->line_overflow) return;
    if(app->line_len + 1 >= AXD_LINE_MAX) {
        app->line_overflow = true;
        app->line_len = 0;
        return;
    }
    app->line[app->line_len++] = (char)byte;
}

static void drain_rx(AxdApp* app) {
    uint8_t chunk[128];
    size_t n;
    while((n = furi_stream_buffer_receive(app->rx_stream, chunk, sizeof(chunk), 0)) > 0) {
        for(size_t i = 0; i < n; i++) feed_byte(app, chunk[i]);
    }
}

static void arm_scan(AxdApp* app, bool reset_retries) {
    if(reset_retries) app->scan_retries = 0;
    app->scan_active = true;
    app->scan_cancel = false;
    app->saw_passive = false;
    app->last_ap_ms = 0;
    app->in_gap = false;
    app->scan_t0 = furi_get_tick();
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->model.scanning = true;
    app->model.clients_on = false;
    status_unlocked(app, "Scanning...");
    furi_mutex_release(app->mutex);
    tx_scan_burst(app);
}

static void finish_scan(AxdApp* app, const char* text) {
    bool cancelled = app->scan_cancel;
    app->scan_active = false;
    app->saw_passive = false;
    app->in_gap = false;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    int count = app->model.ap_count;
    app->model.scanning = false;
    if(cancelled) {
        app->model.ap_log = false;
        status_unlocked(app, "Scan stopped");
    } else if(text) {
        status_unlocked(app, text);
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "%d access points", count);
        status_unlocked(app, buf);
    }
    furi_mutex_release(app->mutex);
    if(cancelled) {
        app->ap_log_want = false;
        close_log(app);
        return;
    }
    if(app->ap_log_want) {
        app->in_gap = true;
        app->gap_until = furi_get_tick() + 2000;
    }
}

static void advance_scan(AxdApp* app) {
    uint32_t now = furi_get_tick();
    if(app->in_gap && (int32_t)(now - app->gap_until) >= 0) {
        app->in_gap = false;
        if(app->ap_log_want && !app->scan_cancel) arm_scan(app, true);
        return;
    }
    if(!app->scan_active) return;
    if(!app->saw_passive && (int32_t)(now - app->scan_t0) > 4000) {
        if(app->scan_retries < 1) {
            app->scan_retries++;
            arm_scan(app, false);
        } else {
            finish_scan(app, "Bridge did not rescan");
        }
        return;
    }
    if(app->saw_passive && app->last_ap_ms && (int32_t)(now - app->last_ap_ms) > 800) {
        finish_scan(app, NULL);
        return;
    }
    if(app->saw_passive && !app->last_ap_ms && (int32_t)(now - app->scan_t0) > 12000) {
        finish_scan(app, "0 access points");
        return;
    }
    if(app->saw_passive && (int32_t)(now - app->scan_t0) > 30000) {
        finish_scan(app, NULL);
    }
}

static void load_log_view(AxdApp* app);
static void export_log(AxdApp* app);

static bool reject_if_scanning(AxdApp* app) {
    if(!app->scan_active && !app->in_gap) return false;
    set_status(app, "Wait for the scan to finish");
    return true;
}

static void handle_msg(AxdApp* app, uint8_t id) {
    bool bridge = false;
    bool clients = false;
    bool nmea = false;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    bridge = app->model.bridge_wd;
    clients = app->model.clients_on;
    nmea = app->model.nmea_on;
    furi_mutex_release(app->mutex);

    switch(id) {
    case AxdMsgRefresh:
        if(bridge) {
            set_status(app, "Stop bridge wardrive first");
            return;
        }
        if(app->scan_active) return;
        app->in_gap = false;
        arm_scan(app, true);
        break;
    case AxdMsgStopScan:
        app->scan_cancel = true;
        app->ap_log_want = false;
        app->in_gap = false;
        if(!app->scan_active) {
            tx_op(app, AXD_OP_STOP);
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->model.scanning = false;
            app->model.ap_log = false;
            app->model.clients_on = false;
            status_unlocked(app, "Stop sent");
            furi_mutex_release(app->mutex);
            close_log(app);
        } else {
            set_status(app, "Stopping after this scan");
        }
        break;
    case AxdMsgClientsStart:
        if(reject_if_scanning(app) || bridge) {
            if(bridge) set_status(app, "Stop bridge wardrive first");
            return;
        }
        tx_op(app, AXD_OP_CLIENTS);
        set_status(app, "Starting client sniffer");
        break;
    case AxdMsgClientsStop:
        tx_op(app, AXD_OP_STOP);
        set_status(app, "Stopping client sniffer");
        break;
    case AxdMsgApLogStart:
        if(bridge) {
            set_status(app, "Stop bridge wardrive first");
            return;
        }
        if(app->scan_active) return;
        if(!open_log(app)) return;
        app->ap_log_want = true;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->model.ap_log = true;
        furi_mutex_release(app->mutex);
        arm_scan(app, true);
        break;
    case AxdMsgApLogStop:
        app->ap_log_want = false;
        app->scan_cancel = true;
        app->in_gap = false;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->model.ap_log = false;
        status_unlocked(app, app->scan_active ? "Stopping log" : "AP log stopped");
        furi_mutex_release(app->mutex);
        if(!app->scan_active) {
            tx_op(app, AXD_OP_STOP);
            close_log(app);
        }
        break;
    case AxdMsgBridgeStart:
        if(reject_if_scanning(app)) return;
        app->ap_log_want = false;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->model.ap_log = false;
        furi_mutex_release(app->mutex);
        bool logged = open_log(app);
        tx_op(app, AXD_OP_STOP);
        tx_op(app, AXD_OP_WARDRIVE_START);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->model.bridge_wd = true;
        status_unlocked(app, logged ? "Bridge wardrive started" : "Wardrive on, SD failed");
        furi_mutex_release(app->mutex);
        break;
    case AxdMsgBridgeStop:
        tx_op(app, AXD_OP_WARDRIVE_STOP);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->model.bridge_wd = false;
        status_unlocked(app, "Bridge wardrive stop sent");
        furi_mutex_release(app->mutex);
        close_log(app);
        break;
    case AxdMsgNmea:
        if(app->scan_active || bridge || clients) {
            set_status(app, "Toggle GPS before a tool");
            return;
        }
        tx_bytes(app, "r");
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->model.nmea_on = !nmea;
        status_unlocked(app, app->model.nmea_on ? "GPS NMEA on" : "GPS NMEA off");
        furi_mutex_release(app->mutex);
        break;
    case AxdMsgFleetStart:
        if(reject_if_scanning(app)) return;
        tx_op(app, AXD_OP_STOP);
        tx_op(app, AXD_OP_FLEET_START);
        set_status(app, "Starting fleet");
        break;
    case AxdMsgFleetJoin:
        if(reject_if_scanning(app)) return;
        tx_op(app, AXD_OP_STOP);
        tx_op(app, AXD_OP_FLEET_JOIN);
        set_status(app, "Joining fleet");
        break;
    case AxdMsgFleetLeave:
        tx_op(app, AXD_OP_FLEET_LEAVE);
        set_status(app, "Leaving fleet");
        break;
    case AxdMsgLoadLog:
        load_log_view(app);
        break;
    case AxdMsgExport:
        export_log(app);
        break;
    default:
        break;
    }
}

static void load_log_view(AxdApp* app) {
    char path[96];
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    snprintf(path, sizeof(path), "%s", app->model.log_path);
    furi_mutex_release(app->mutex);
    if(!path[0]) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        snprintf(app->model.log_text, sizeof(app->model.log_text), "No log yet");
        app->model.log_dirty = true;
        furi_mutex_release(app->mutex);
        return;
    }
    File* file = storage_file_alloc(app->storage);
    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        snprintf(app->model.log_text, sizeof(app->model.log_text), "Cannot open log");
        app->model.log_dirty = true;
        furi_mutex_release(app->mutex);
        return;
    }
    uint64_t size = storage_file_size(file);
    uint32_t start = 0;
    if(size > 1300) start = (uint32_t)(size - 1300);
    storage_file_seek(file, start, true);
    char buf[1400];
    size_t n = storage_file_read(file, buf, sizeof(buf) - 1);
    buf[n] = 0;
    storage_file_close(file);
    storage_file_free(file);
    char* body = buf;
    if(start && (body = strchr(buf, '\n'))) body++;
    if(!body) body = buf;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    snprintf(app->model.log_text, sizeof(app->model.log_text), "%s", body);
    app->model.log_dirty = true;
    furi_mutex_release(app->mutex);
}

static void export_log(AxdApp* app) {
    char path[96];
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    snprintf(path, sizeof(path), "%s", app->model.log_path);
    furi_mutex_release(app->mutex);
    if(!path[0]) {
        set_status(app, "No log yet");
        return;
    }
    storage_common_mkdir(app->storage, "/ext/apps_data/axd_recon/export");
    const char* dest = "/ext/apps_data/axd_recon/export/wardrive.csv";
    File* in = storage_file_alloc(app->storage);
    File* out = storage_file_alloc(app->storage);
    bool ok = storage_file_open(in, path, FSAM_READ, FSOM_OPEN_EXISTING) &&
              storage_file_open(out, dest, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        uint8_t buf[256];
        size_t n;
        while((n = storage_file_read(in, buf, sizeof(buf))) > 0) {
            if(storage_file_write(out, buf, n) != n) {
                ok = false;
                break;
            }
        }
    }
    storage_file_close(in);
    storage_file_close(out);
    storage_file_free(in);
    storage_file_free(out);
    set_status(app, ok ? "Exported export/wardrive.csv" : "Export failed");
}

static int32_t worker_main(void* context) {
    AxdApp* app = context;
    while(!app->exit_req) {
        AxdMsg msg;
        FuriStatus status = furi_message_queue_get(app->queue, &msg, 20);
        if(status == FuriStatusOk) {
            if(msg.id == AxdMsgExit || app->exit_req) break;
            handle_msg(app, msg.id);
        }
        drain_rx(app);
        advance_scan(app);
    }
    return 0;
}

static void rx_callback(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    AxdApp* app = context;
    if(!(event & FuriHalSerialRxEventData)) return;
    while(furi_hal_serial_async_rx_available(handle)) {
        uint8_t byte = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(app->rx_stream, &byte, 1, 0);
    }
}

static void open_uart(AxdApp* app) {
    app->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(app->expansion);
    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(!app->serial) {
        set_status(app, "USART busy");
        return;
    }
    furi_hal_serial_init(app->serial, AXD_BAUD);
    furi_hal_serial_async_rx_start(app->serial, rx_callback, app, false);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->model.uart_ok = true;
    status_unlocked(app, "USART 13/14 115200");
    furi_mutex_release(app->mutex);
}

AxdApp* axd_app_alloc(void) {
    AxdApp* app = malloc(sizeof(AxdApp));
    memset(app, 0, sizeof(AxdApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(AxdMsg));
    app->rx_stream = furi_stream_buffer_alloc(AXD_RX_STREAM, 1);
    app->storage = furi_record_open(RECORD_STORAGE);
    snprintf(app->model.status, sizeof(app->model.status), "Starting UART");
    snprintf(app->model.log_text, sizeof(app->model.log_text), "No log yet");
    app->worker = furi_thread_alloc_ex("AxdBridge", 4096, worker_main, app);
    furi_thread_start(app->worker);
    open_uart(app);
    return app;
}

void axd_app_free(AxdApp* app) {
    if(!app) return;
    app->exit_req = true;
    axd_app_post(app, AxdMsgExit);
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    if(app->serial) {
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
    }
    if(app->expansion) {
        expansion_enable(app->expansion);
        furi_record_close(RECORD_EXPANSION);
    }
    close_log(app);
    if(app->storage) furi_record_close(RECORD_STORAGE);
    furi_stream_buffer_free(app->rx_stream);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);
}

void axd_app_post(AxdApp* app, uint8_t id) {
    AxdMsg msg = {.id = id};
    furi_message_queue_put(app->queue, &msg, 0);
}

void axd_app_lock(AxdApp* app) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
}

void axd_app_unlock(AxdApp* app) {
    furi_mutex_release(app->mutex);
}

AxdModel* axd_app_model(AxdApp* app) {
    return &app->model;
}
