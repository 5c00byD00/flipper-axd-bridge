#include "axd_protocol.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect(int cond, const char* msg) {
    if(!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        failures++;
    }
}

static void test_commands(void) {
    char buf[24];
    expect(axd_format_command(buf, sizeof(buf), AXD_OP_WIFI_SCAN), "wifi format");
    expect(strcmp(buf, "$CMD,1,0\n") == 0, "wifi text");
    expect(axd_format_command(buf, sizeof(buf), AXD_OP_CHANNEL_MAP), "map format");
    expect(strcmp(buf, "$CMD,3,0\n") == 0, "map text");
    expect(axd_format_command(buf, sizeof(buf), AXD_OP_CLIENTS), "clients");
    expect(axd_format_command(buf, sizeof(buf), AXD_OP_WARDRIVE_START), "wd start");
    expect(axd_format_command(buf, sizeof(buf), AXD_OP_WARDRIVE_STOP), "wd stop");
    expect(axd_format_command(buf, sizeof(buf), AXD_OP_FLEET_START), "fleet start");
    expect(strcmp(buf, "$CMD,60,0\n") == 0, "fleet start text");
    expect(axd_format_command(buf, sizeof(buf), AXD_OP_FLEET_JOIN), "fleet join");
    expect(axd_format_command(buf, sizeof(buf), AXD_OP_FLEET_LEAVE), "fleet leave");
    expect(axd_format_command(buf, sizeof(buf), AXD_OP_STOP), "stop");
    expect(strcmp(buf, "$CMD,255,0\n") == 0, "stop text");
    expect(!axd_format_command(buf, sizeof(buf), 0), "reject 0");
    expect(!axd_format_command(buf, sizeof(buf), 2), "reject 2");
    expect(!axd_format_command(buf, sizeof(buf), 4), "reject 4");
    expect(!axd_format_command(buf, sizeof(buf), 20), "reject 20");
    expect(!axd_format_command(buf, sizeof(buf), 52), "reject 52");
    expect(!axd_format_command(buf, sizeof(buf), 63), "reject 63");
}

static void test_wifi_line(void) {
    AxdEvent ev;
    const char* line = "[wifi]  -42 dBm ch6   AA:BB:CC:DD:EE:FF Cafe WiFi";
    expect(axd_parse_line(line, &ev), "parse ap");
    expect(ev.type == AxdEvtWifiAp, "type ap");
    expect(ev.rssi == -42, "rssi");
    expect(ev.channel == 6, "channel");
    expect(strcmp(ev.bssid, "AA:BB:CC:DD:EE:FF") == 0, "bssid");
    expect(strcmp(ev.ssid, "Cafe WiFi") == 0, "ssid");
    expect(ev.auth[0] == 0, "auth absent");

    expect(
        axd_parse_line("[wifi] -100 dBm ch100 11:22:33:44:55:66", &ev),
        "parse 5ghz hidden");
    expect(ev.channel == 100, "ch 100");
    expect(ev.ssid[0] == 0, "hidden ssid");
    expect(ev.rssi == -100, "rssi -100");

    expect(
        axd_parse_line("[wifi] passive access-point scan started", &ev) &&
            ev.type == AxdEvtWifiPassiveStarted,
        "passive");
    expect(
        axd_parse_line("[wifi] continuous scan started", &ev) &&
            ev.type == AxdEvtWifiContinuousStarted,
        "continuous");
    expect(
        axd_parse_line("[wifi] scan failed (-1); previous results retained", &ev) &&
            ev.type == AxdEvtWifiScanFailed && ev.count == -1,
        "failed");
}

static void test_clients_and_remote(void) {
    AxdEvent ev;
    expect(
        axd_parse_line("[clients] starting probe-request sniffer", &ev) &&
            ev.type == AxdEvtClientsStarted,
        "clients start");
    expect(
        axd_parse_line("[clients] stopped; 14 client(s)", &ev) &&
            ev.type == AxdEvtClientsStopped && ev.count == 14,
        "clients stop");
    expect(
        axd_parse_line("[remote] command op=3 arg=0", &ev) && ev.type == AxdEvtRemoteCmd &&
            ev.op == 3 && ev.arg == 0,
        "remote");
}

static void test_wdstat(void) {
    AxdEvent ev;
    const char* line =
        "$WDSTAT,1,0,1234,1,95,120,80,12,3,4,8,1.2,0,40,800,2,"
        "2026-11-26 12:00:00,GMT-05:00 standard,wardrive-0007.csv,0,1,3.5,1";
    expect(axd_parse_line(line, &ev), "wdstat parse");
    expect(ev.type == AxdEvtWdStat, "wdstat type");
    expect(ev.active == 1, "active");
    expect(ev.seconds == 95, "seconds");
    expect(ev.metres > 119.0 && ev.metres < 121.0, "metres");
    expect(ev.fix_pct == 80, "fix pct");
    expect(ev.wifi == 12 && ev.ble == 3, "counts");
    expect(ev.sats == 8, "sats");
    expect(ev.hdop > 1.1 && ev.hdop < 1.3, "hdop");
    expect(ev.rows == 40, "rows");
    expect(strcmp(ev.text, "wardrive-0007.csv") == 0, "file");
    expect(ev.gps_fix_flag == 1, "fix flag");
    expect(ev.speed_kmh > 3.4 && ev.speed_kmh < 3.6, "speed");
}

static void test_nmea(void) {
    AxdEvent ev;
    expect(
        axd_parse_line("$GNGGA,123519.00,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47", &ev),
        "gga");
    expect(ev.type == AxdEvtNmea && ev.gps_valid, "gga valid");
    expect(strcmp(ev.text, "GGA") == 0, "gga tag");
    expect(fabs(ev.lat - 48.1173) < 0.001, "lat");
    expect(fabs(ev.lon - 11.516666) < 0.001, "lon");
    expect(ev.sats == 8, "gga sats");
    expect(ev.alt > 545.0 && ev.alt < 546.0, "alt");

    expect(axd_parse_line("$GPRMC,123519,A,4807.038,S,01131.000,W,022.4,084.4,230394,003.1,W*6A", &ev), "rmc");
    expect(ev.gps_valid && ev.lat < 0 && ev.lon < 0, "rmc hemi");
    expect(strcmp(ev.text, "RMC") == 0, "rmc tag");
}

static void test_fleet(void) {
    AxdEvent ev;
    expect(
        axd_parse_line("[fleet] armed; listening for an invite", &ev) && ev.type == AxdEvtFleetArmed,
        "armed");
    expect(
        axd_parse_line("[fleet] coordinator up, session 305419896 code 0042", &ev) &&
            ev.type == AxdEvtFleetCoordinator && ev.session == 305419896u && ev.code == 42,
        "coord");
    expect(
        axd_parse_line("[fleet] roster N=3 myIndex=1 role=WIFI wifiSlice=0/2 coord=0", &ev) &&
            ev.type == AxdEvtFleetRoster && ev.count == 3 && ev.index == 1 &&
            strcmp(ev.role, "WIFI") == 0 && ev.slice == 0 && ev.slice_count == 2 && ev.coord == 0,
        "roster");
    expect(
        axd_parse_line("[fleet] N=3 agg wifi=10 ble=2 | rows m0=1 m1=4", &ev) &&
            ev.type == AxdEvtFleetAgg && ev.wifi == 10 && ev.ble == 2,
        "agg");
    expect(
        axd_parse_line("[fleet] member 2 joined", &ev) && ev.type == AxdEvtFleetJoined &&
            ev.index == 2,
        "joined");
    expect(
        axd_parse_line("[fleet] member 1 timed out", &ev) && ev.type == AxdEvtFleetTimeout &&
            ev.index == 1,
        "timeout");
    expect(axd_parse_line("[fleet] left the session", &ev) && ev.type == AxdEvtFleetLeft, "left");
    expect(
        axd_parse_line("[link] split wardrive started (solo, 38 channels)", &ev) &&
            ev.type == AxdEvtLinkStarted,
        "link start");
    expect(
        axd_parse_line("[link] split wardrive stopped; 9 Wi-Fi networks", &ev) &&
            ev.type == AxdEvtLinkStopped && ev.wifi == 9,
        "link stop");
    expect(
        axd_parse_line("[wardrive] stopped; 11 Wi-Fi, 4 BLE", &ev) &&
            ev.type == AxdEvtWardriveStopped && ev.wifi == 11 && ev.ble == 4,
        "wd stop");
}

static void test_wigle(void) {
    AxdEvent ev;
    const char* line =
        "AA:BB:CC:DD:EE:FF,Cafe,[WPA2-PSK-CCMP][ESS],2026-11-26 12:00:00,6,2437,-40,"
        "48.117300,11.516666,545,,,0,WIFI";
    expect(axd_parse_line(line, &ev), "wigle");
    expect(ev.type == AxdEvtWigleRow, "wigle type");
    expect(strcmp(ev.auth, "[WPA2-PSK-CCMP][ESS]") == 0, "auth");
    expect(ev.channel == 6 && ev.rssi == -40, "wigle radio");
    expect(ev.gps_valid, "wigle gps");
    expect(strcmp(ev.ssid, "Cafe") == 0, "wigle ssid");
}

static void test_ignored(void) {
    AxdEvent ev;
    expect(!axd_parse_line("AxD starting", &ev), "banner not an event");
    expect(!axd_parse_line("", &ev), "empty");
}

int main(void) {
    test_commands();
    test_wifi_line();
    test_clients_and_remote();
    test_wdstat();
    test_nmea();
    test_fleet();
    test_wigle();
    test_ignored();
    if(failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
