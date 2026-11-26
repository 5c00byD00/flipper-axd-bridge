#include "axd_protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_str(char* dst, size_t n, const char* src) {
    if(!dst || n == 0) return;
    if(!src) src = "";
    snprintf(dst, n, "%s", src);
}

static void event_clear(AxdEvent* out) {
    memset(out, 0, sizeof(*out));
}

static bool allowed_op(unsigned op) {
    switch(op) {
    case AXD_OP_WIFI_SCAN:
    case AXD_OP_CHANNEL_MAP:
    case AXD_OP_CLIENTS:
    case AXD_OP_WARDRIVE_START:
    case AXD_OP_WARDRIVE_STOP:
    case AXD_OP_FLEET_START:
    case AXD_OP_FLEET_JOIN:
    case AXD_OP_FLEET_LEAVE:
    case AXD_OP_STOP:
        return true;
    default:
        return false;
    }
}

bool axd_format_command(char* out, size_t out_len, unsigned op) {
    if(!out || out_len < 12 || !allowed_op(op)) return false;
    int wrote = snprintf(out, out_len, "$CMD,%u,0\n", op);
    return wrote > 0 && (size_t)wrote < out_len;
}

static bool starts(const char* line, const char* prefix) {
    return strncmp(line, prefix, strlen(prefix)) == 0;
}

static const char* skip_sp(const char* p) {
    while(*p == ' ') p++;
    return p;
}

static bool parse_wifi_ap(const char* line, AxdEvent* out) {
    // AWOKxDAG.ino scanWifi(): "[wifi] %4ld dBm ch%-3ld %s %s"
    const char* p = line + strlen("[wifi] ");
    p = skip_sp(p);
    char* end = NULL;
    long rssi = strtol(p, &end, 10);
    if(end == p || strncmp(end, " dBm ch", 7) != 0) return false;
    p = end + 7;
    long channel = strtol(p, &end, 10);
    if(end == p) return false;
    p = skip_sp(end);
    char bssid[18];
    size_t i = 0;
    while(*p && *p != ' ' && i + 1 < sizeof(bssid)) {
        bssid[i++] = *p++;
    }
    bssid[i] = 0;
    if(i < 17 || bssid[2] != ':') return false;
    if(*p == ' ') p++;
    while(*p == ' ') p++;
    event_clear(out);
    out->type = AxdEvtWifiAp;
    out->rssi = (int)rssi;
    out->channel = (int)channel;
    copy_str(out->bssid, sizeof(out->bssid), bssid);
    copy_str(out->ssid, sizeof(out->ssid), p);
    return true;
}

static int split_commas(char* line, char** fields, int max_fields) {
    int count = 0;
    if(max_fields <= 0) return 0;
    fields[count++] = line;
    for(char* p = line; *p && count < max_fields; p++) {
        if(*p == ',') {
            *p = 0;
            fields[count++] = p + 1;
        }
    }
    return count;
}

static bool mac_token(const char* s) {
    if(!s) return false;
    for(int i = 0; i < 17; i++) {
        if(i % 3 == 2) {
            if(s[i] != ':') return false;
        } else if(!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return s[17] == 0;
}

static double axd_number(const char* text) {
    if(!text || !text[0]) return (double)0;
    int sign = 1;
    if(*text == '-') {
        sign = -1;
        text++;
    } else if(*text == '+') {
        text++;
    }
    double whole = (double)0;
    while(*text >= '0' && *text <= '9') {
        whole = whole * (double)10 + (double)(*text - '0');
        text++;
    }
    double frac = (double)0;
    double place = (double)1;
    if(*text == '.') {
        text++;
        while(*text >= '0' && *text <= '9') {
            place *= (double)10;
            frac = frac * (double)10 + (double)(*text - '0');
            text++;
        }
    }
    double value = whole + frac / place;
    return sign < 0 ? -value : value;
}

static double nmea_deg(const char* field, const char* hemi) {
    if(!field || !field[0]) return (double)0;
    double raw = axd_number(field);
    int degrees = (int)(raw / (double)100);
    double minutes = raw - (double)degrees * (double)100;
    double value = (double)degrees + minutes / (double)60;
    if(hemi && (hemi[0] == 'S' || hemi[0] == 'W')) value = -value;
    return value;
}

static bool parse_nmea(char* line, AxdEvent* out) {
    char* fields[20];
    int count = split_commas(line, fields, 20);
    if(count < 7 || fields[0][0] != '$' || strlen(fields[0]) < 6) return false;
    // Trim checksum from the last field we care about later; strip '*' now.
    for(int i = 0; i < count; i++) {
        char* star = strchr(fields[i], '*');
        if(star) *star = 0;
    }
    const char* kind = fields[0] + 3;
    if(strcmp(kind, "GGA") == 0 && count >= 10) {
        int quality = atoi(fields[6]);
        event_clear(out);
        out->type = AxdEvtNmea;
        out->gps_valid = quality > 0 && fields[2][0] && fields[4][0];
        out->lat = nmea_deg(fields[2], fields[3]);
        out->lon = nmea_deg(fields[4], fields[5]);
        out->sats = atoi(fields[7]);
        out->hdop = axd_number(fields[8]);
        out->alt = axd_number(fields[9]);
        copy_str(out->text, sizeof(out->text), "GGA");
        return true;
    }
    if(strcmp(kind, "RMC") == 0 && count >= 7) {
        event_clear(out);
        out->type = AxdEvtNmea;
        out->gps_valid = fields[2][0] == 'A' && fields[3][0] && fields[5][0];
        out->lat = nmea_deg(fields[3], fields[4]);
        out->lon = nmea_deg(fields[5], fields[6]);
        if(count > 7 && fields[7][0]) {
            out->speed_kmh = axd_number(fields[7]) * ((double)1852 / (double)1000);
        }
        copy_str(out->text, sizeof(out->text), "RMC");
        return true;
    }
    return false;
}

static void unquote(char* s) {
    size_t n = strlen(s);
    if(n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = 0;
        memmove(s, s + 1, n - 1);
    }
}

static bool parse_wigle(char* line, AxdEvent* out) {
    // Wardrive WiGLE body from gps.ino appendWardriveRow / wardriveEmitPeerRow.
    // dual-c5-bridge 1.7.8 does not print these on UART; accept them if a build does.
    char* fields[16];
    int count = split_commas(line, fields, 16);
    if(count < 8 || !mac_token(fields[0])) return false;
    unquote(fields[1]);
    const char* type = fields[count - 1];
    if(strcmp(type, "WIFI") != 0 && strcmp(type, "BLE") != 0) return false;
    event_clear(out);
    out->type = AxdEvtWigleRow;
    copy_str(out->bssid, sizeof(out->bssid), fields[0]);
    copy_str(out->ssid, sizeof(out->ssid), fields[1]);
    copy_str(out->auth, sizeof(out->auth), fields[2]);
    out->channel = atoi(fields[4]);
    out->rssi = atoi(fields[6]);
    if(fields[7][0] && fields[8][0]) {
        out->gps_valid = true;
        out->lat = axd_number(fields[7]);
        out->lon = axd_number(fields[8]);
        if(count > 9 && fields[9][0]) out->alt = axd_number(fields[9]);
    }
    return true;
}

static bool parse_wdstat(char* line, AxdEvent* out) {
    // gps.ino broadcastWardriveDashboard(), version 1.
    char* fields[24];
    int count = split_commas(line, fields, 24);
    if(count < 22 || strcmp(fields[0], "$WDSTAT") != 0 || strcmp(fields[1], "1") != 0) {
        return false;
    }
    event_clear(out);
    out->type = AxdEvtWdStat;
    out->active = (unsigned)strtoul(fields[4], NULL, 10);
    out->seconds = (uint32_t)strtoul(fields[5], NULL, 10);
    out->metres = axd_number(fields[6]);
    out->fix_pct = (unsigned)strtoul(fields[7], NULL, 10);
    out->wifi = (uint32_t)strtoul(fields[8], NULL, 10);
    out->ble = (uint32_t)strtoul(fields[9], NULL, 10);
    out->sats = atoi(fields[11]);
    out->hdop = axd_number(fields[12]);
    out->storage = atoi(fields[13]);
    out->rows = (uint32_t)strtoul(fields[14], NULL, 10);
    out->gps_fix_flag = (unsigned)strtoul(fields[21], NULL, 10);
    if(count > 22) out->speed_kmh = axd_number(fields[22]);
    if(count > 19) copy_str(out->text, sizeof(out->text), fields[19]);
    return true;
}

bool axd_parse_line(const char* line, AxdEvent* out) {
    if(!out) return false;
    event_clear(out);
    if(!line || !line[0]) return false;

    if(strcmp(line, "[wifi] continuous scan started") == 0) {
        out->type = AxdEvtWifiContinuousStarted;
        return true;
    }
    if(strcmp(line, "[wifi] continuous scan stopped") == 0) {
        out->type = AxdEvtWifiContinuousStopped;
        return true;
    }
    if(strcmp(line, "[wifi] passive access-point scan started") == 0) {
        out->type = AxdEvtWifiPassiveStarted;
        return true;
    }
    if(starts(line, "[wifi] scan failed")) {
        out->type = AxdEvtWifiScanFailed;
        sscanf(line, "[wifi] scan failed (%d)", &out->count);
        return true;
    }
    if(starts(line, "[wifi] ") && parse_wifi_ap(line, out)) return true;

    if(strcmp(line, "[clients] starting probe-request sniffer") == 0) {
        out->type = AxdEvtClientsStarted;
        return true;
    }
    if(starts(line, "[clients] stopped;")) {
        out->type = AxdEvtClientsStopped;
        sscanf(line, "[clients] stopped; %d", &out->count);
        return true;
    }

    if(starts(line, "[wardrive] started")) {
        out->type = AxdEvtWardriveStarted;
        copy_str(out->text, sizeof(out->text), line + strlen("[wardrive] started "));
        return true;
    }
    if(starts(line, "[wardrive] stopped;")) {
        unsigned long wifi = 0;
        unsigned long ble = 0;
        out->type = AxdEvtWardriveStopped;
        sscanf(line, "[wardrive] stopped; %lu Wi-Fi, %lu BLE", &wifi, &ble);
        out->wifi = (uint32_t)wifi;
        out->ble = (uint32_t)ble;
        return true;
    }
    if(starts(line, "[wardrive] ")) {
        out->type = AxdEvtWardriveNote;
        copy_str(out->text, sizeof(out->text), line);
        return true;
    }

    if(starts(line, "$WDSTAT,")) {
        char buf[320];
        snprintf(buf, sizeof(buf), "%s", line);
        return parse_wdstat(buf, out);
    }
    if(line[0] == '$' && (strstr(line, "GGA,") || strstr(line, "RMC,"))) {
        char buf[200];
        snprintf(buf, sizeof(buf), "%s", line);
        return parse_nmea(buf, out);
    }

    if(strcmp(line, "[fleet] armed; listening for an invite") == 0) {
        out->type = AxdEvtFleetArmed;
        return true;
    }
    if(starts(line, "[fleet] coordinator up,")) {
        unsigned long session = 0;
        unsigned code = 0;
        if(sscanf(line, "[fleet] coordinator up, session %lu code %u", &session, &code) == 2) {
            out->type = AxdEvtFleetCoordinator;
            out->session = (uint32_t)session;
            out->code = code;
            return true;
        }
    }
    if(starts(line, "[fleet] member ") && strstr(line, "timed out")) {
        out->type = AxdEvtFleetTimeout;
        sscanf(line, "[fleet] member %d timed out", &out->index);
        return true;
    }
    if(starts(line, "[fleet] member ") && strstr(line, "joined")) {
        out->type = AxdEvtFleetJoined;
        sscanf(line, "[fleet] member %d joined", &out->index);
        return true;
    }
    if(starts(line, "[fleet] roster ")) {
        out->type = AxdEvtFleetRoster;
        sscanf(
            line,
            "[fleet] roster N=%d myIndex=%d role=%7s wifiSlice=%d/%d coord=%d",
            &out->count,
            &out->index,
            out->role,
            &out->slice,
            &out->slice_count,
            &out->coord);
        return true;
    }
    if(starts(line, "[fleet] N=") && strstr(line, "agg wifi=")) {
        unsigned long wifi = 0;
        unsigned long ble = 0;
        out->type = AxdEvtFleetAgg;
        sscanf(line, "[fleet] N=%d agg wifi=%lu ble=%lu", &out->count, &wifi, &ble);
        out->wifi = (uint32_t)wifi;
        out->ble = (uint32_t)ble;
        return true;
    }
    if(strcmp(line, "[fleet] left the session") == 0) {
        out->type = AxdEvtFleetLeft;
        return true;
    }
    if(strcmp(line, "[fleet] BLE node scanning") == 0) {
        out->type = AxdEvtFleetBle;
        return true;
    }
    if(starts(line, "[fleet] ")) {
        out->type = AxdEvtFleetNote;
        copy_str(out->text, sizeof(out->text), line);
        return true;
    }

    if(starts(line, "[link] split wardrive started")) {
        out->type = AxdEvtLinkStarted;
        copy_str(out->text, sizeof(out->text), line);
        return true;
    }
    if(starts(line, "[link] split wardrive stopped;")) {
        unsigned long wifi = 0;
        out->type = AxdEvtLinkStopped;
        sscanf(line, "[link] split wardrive stopped; %lu", &wifi);
        out->wifi = (uint32_t)wifi;
        return true;
    }
    if(starts(line, "[remote] command op=")) {
        out->type = AxdEvtRemoteCmd;
        sscanf(line, "[remote] command op=%u arg=%u", &out->op, &out->arg);
        return true;
    }
    if(starts(line, "[sd] ")) {
        out->type = AxdEvtSdNote;
        copy_str(out->text, sizeof(out->text), line);
        return true;
    }

    if(isxdigit((unsigned char)line[0])) {
        char buf[320];
        snprintf(buf, sizeof(buf), "%s", line);
        if(parse_wigle(buf, out)) return true;
    }
    return false;
}
