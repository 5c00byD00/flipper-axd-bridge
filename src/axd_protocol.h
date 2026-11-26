#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Passive UART commands for the AxD headless bridge.
// Verified against dagnazty/awokxdag @ fe02853 (firmware 1.7.8):
//   AWOKxDAG/AWOKxDAG.ino          Serial.begin(115200), setup banner
//   AWOKxDAG/input.ino             handleSerial(): "$CMD,<op>,<arg>\n"
//   AWOKxDAG/link_protocol.h       AxdCommand opcode values
//   AWOKxDAG/link.ino              linkDispatchCommand()
// The formatter refuses every opcode outside this passive set.

#define AXD_BAUD 115200

#define AXD_OP_WIFI_SCAN 1
#define AXD_OP_CHANNEL_MAP 3
#define AXD_OP_CLIENTS 6
#define AXD_OP_WARDRIVE_START 7
#define AXD_OP_WARDRIVE_STOP 8
#define AXD_OP_FLEET_START 60
#define AXD_OP_FLEET_JOIN 61
#define AXD_OP_FLEET_LEAVE 62
#define AXD_OP_STOP 255

typedef enum {
    AxdEvtNone = 0,
    AxdEvtWifiAp,
    AxdEvtWifiPassiveStarted,
    AxdEvtWifiContinuousStarted,
    AxdEvtWifiContinuousStopped,
    AxdEvtWifiScanFailed,
    AxdEvtClientsStarted,
    AxdEvtClientsStopped,
    AxdEvtWardriveStarted,
    AxdEvtWardriveStopped,
    AxdEvtWardriveNote,
    AxdEvtWdStat,
    AxdEvtNmea,
    AxdEvtFleetArmed,
    AxdEvtFleetCoordinator,
    AxdEvtFleetJoined,
    AxdEvtFleetRoster,
    AxdEvtFleetAgg,
    AxdEvtFleetLeft,
    AxdEvtFleetTimeout,
    AxdEvtFleetBle,
    AxdEvtFleetNote,
    AxdEvtLinkStarted,
    AxdEvtLinkStopped,
    AxdEvtRemoteCmd,
    AxdEvtWigleRow,
    AxdEvtSdNote,
} AxdEvtType;

typedef struct {
    AxdEvtType type;
    int rssi;
    int channel;
    int count;
    int index;
    int slice;
    int slice_count;
    int coord;
    unsigned op;
    unsigned arg;
    unsigned code;
    uint32_t session;
    uint32_t seconds;
    uint32_t wifi;
    uint32_t ble;
    uint32_t rows;
    unsigned fix_pct;
    unsigned active;
    unsigned gps_fix_flag;
    int sats;
    int storage;
    double metres;
    double hdop;
    double speed_kmh;
    double lat;
    double lon;
    double alt;
    bool gps_valid;
    char ssid[33];
    char bssid[18];
    char auth[40];
    char role[8];
    char text[48];
} AxdEvent;

bool axd_format_command(char* out, size_t out_len, unsigned op);
bool axd_parse_line(const char* line, AxdEvent* out);
