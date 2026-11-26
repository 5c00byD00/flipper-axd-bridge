#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AXD_MAX_APS 36
#define AXD_SERIAL_LINES 24
#define AXD_SERIAL_COLS 40

enum {
    AxdMsgRefresh = 1,
    AxdMsgStopScan,
    AxdMsgClientsStart,
    AxdMsgClientsStop,
    AxdMsgApLogStart,
    AxdMsgApLogStop,
    AxdMsgBridgeStart,
    AxdMsgBridgeStop,
    AxdMsgNmea,
    AxdMsgFleetStart,
    AxdMsgFleetJoin,
    AxdMsgFleetLeave,
    AxdMsgLoadLog,
    AxdMsgExport,
    AxdMsgExit,
};

typedef struct {
    char ssid[33];
    char bssid[18];
    char auth[40];
    int16_t rssi;
    int16_t channel;
} AxdAp;

typedef struct {
    bool uart_ok;
    bool scanning;
    bool clients_on;
    bool clients_count_valid;
    int client_count;
    bool ap_log;
    bool bridge_wd;
    bool nmea_on;
    bool gps_valid;
    double lat;
    double lon;
    double alt;
    int sats;
    bool wd_valid;
    unsigned wd_active;
    uint32_t wd_seconds;
    double wd_metres;
    unsigned wd_fix_pct;
    uint32_t wd_wifi;
    uint32_t wd_ble;
    uint32_t wd_rows;
    int wd_sats;
    double wd_hdop;
    double wd_speed;
    int wd_storage;
    unsigned wd_fix_flag;
    char wd_file[48];
    bool fleet_listening;
    bool fleet_active;
    bool fleet_coordinator;
    bool fleet_ble_node;
    uint32_t fleet_session;
    unsigned fleet_code;
    int fleet_members;
    int fleet_index;
    int fleet_slice;
    int fleet_slice_count;
    int fleet_coord;
    char fleet_role[8];
    uint32_t fleet_wifi;
    uint32_t fleet_ble;
    char fleet_detail[64];
    AxdAp aps[AXD_MAX_APS];
    int ap_count;
    int ap_cursor;
    char status[72];
    char serial_lines[AXD_SERIAL_LINES][AXD_SERIAL_COLS + 1];
    int serial_count;
    int serial_head;
    char serial_text[AXD_SERIAL_LINES * (AXD_SERIAL_COLS + 2)];
    bool serial_dirty;
    char log_path[96];
    char log_text[1500];
    bool log_dirty;
    uint32_t logged_rows;
    bool sd_ok;
} AxdModel;
