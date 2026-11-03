#pragma once
static const char DECODER_JS[] PROGMEM = R"DECODERJS(
/**
 * ChirpStack Payload Decoder – Universal Weather Node
 *
 * Compatible with ChirpStack v3 (Decode function) and v4 (decodeUplink function).
 *
 * Two uplink payload formats, selected by FPort (firmware config `payloadFormat`):
 *   FPort 1 → CayenneLPP (custom channel mapping, see below) — default
 *   FPort 3 → compact custom binary (~50% smaller), see "Custom Binary Decoder" below
 * Both produce the same named JSON fields; downstream consumers don't need
 * to know which format was used on the wire.
 *
 * LPP channel mapping:
 *   Ch 1  Direction        → wind direction (°)
 *   Ch 1  Analog Input     → avg wind speed (m/s or km/h, resolution 0.01)
 *   Ch 2  Analog Input     → wind gust max (m/s or km/h, resolution 0.01)
 *   Ch 3  Analog Input     → rain rate (mm/h, resolution 0.01)
 *   Ch 203 Generic Sensor  → free heap (KiB, debug mode; Analog Input in older firmware)
 *   Ch 204 Generic Sensor  → uptime (0.1 h, debug mode; Analog Input in h in older firmware)
 *   Ch 1  Distance         → rain current cycle (mm or in, resolution 0.001)
 *   Ch 2  Distance         → rain since start (mm or in, resolution 0.001)
 *   Ch 2  Digital Input    → status byte (bitmask, see below)
 *   Ch 200 Digital Input   → cycle counter (0–255, debug mode)
 *   Ch 202 Digital Input   → send fail counter (0–255, debug mode)
 *   Ch 1  Voltage          → LiPo voltage (V, optional)
 *   Ch 1  Rel. Humidity    → bme280 humidity (%, BME280)
 *   Ch 1  Baro. Pressure   → bme280 barometric pressure QNH (hPa; inHg x10, see below)
 *   Ch 2  Baro. Pressure   → bme280 barometric pressure absolute (hPa; inHg x10, see below)
 *   Ch 1  Temperature      → bme280 temperature (°C, BME280)
 *   Ch 2–4 Temperature     → DS18B20 sensor 0–2 (°C)
 *   Ch 201 Temperature     → CPU temperature (°C, debug mode)
 *
 * PCB v2.0 I/O block (each only while the function is enabled):
 *   Ch 3  Digital Input    → io status (bitmask, see decodeIoStatus)
 *   Ch 5  Analog Input     → AUX analog input (V, resolution 0.01)
 *   Ch 10 Generic Sensor   → AUX pulses this cycle
 *   Ch 11 Generic Sensor   → AUX pulses since power-on
 *   Ch 20 Digital Output   → heater power (%)
 *   Ch 21 Digital Output   → fan power (%)
 *   Ch 22 Generic Sensor   → fan speed (rpm)
 *   Ch 12 Voltage          → UV sensor terminal voltage (V)
 *   Ch 13 Analog Input     → calibrated UV index (resolution 0.01)
 *   Ch 14 Digital Input    → UV flags (bit 0 enabled, bit 1 calibrated index)
 *   Ch 30 Voltage          → Victron battery voltage (V)
 *   Ch 30 Analog Input     → Victron battery current (A, negative = discharge)
 *   Ch 31 Analog Input     → Victron load output current (A, MPPT load output)
 *   Ch 31 Voltage          → Victron PV voltage (V)
 *   Ch 30 Power            → Victron PV power (W)
 *   Ch 30 Energy           → Victron yield today (kWh)
 *   Ch 30 Percentage       → Victron state of charge (%, BMV/SmartShunt)
 *   Ch 30 Digital Input    → Victron charge state (CS: 0 off, 3 bulk, 4 absorption, 5 float, ...)
 *   Ch 31 Digital Input    → Victron error code (0 = none)
 *
 * Status byte (Ch 2 Digital Input):
 *   Bit 0 : BME280 present (1=yes)
 *   Bit 1 : 1-Wire has sensors (1=yes)
 *   Bit 2–5: send fail counter (0–15, clipped)
 *   Bit 6 : Imperial units in use (°F, in, in/h, inHg, mph) for ALL of
 *           temperature/rain/pressure/wind in this uplink — values are
 *           already converted, this bit only tells you which unit they're in
 *   Bit 7 : wind speed unit if Metric (0=km/h, 1=m/s; ignored if Bit 6 set)
 *
 * Imperial resolution: in imperial mode the barometric fields carry inHg
 * x100 (the firmware sends inHg x10 into the x10 field) - the decoder
 * divides by 10 once more when Bit 6 is set. The binary format scales
 * rain/pressure by itself (see "Custom Binary Decoder"). Field names keep
 * their historical "_mm"/"_hpa" suffixes; the unit is in status.*_unit.
 *
 * Rain accumulator note:
 *   "rain_since_start_mm" is a continuous counter since the last power-on.
 *   Lost packets do NOT affect accuracy: the delta between two received packets
 *   gives the actual rainfall in that period, even if packets were missed in between.
 *   Example: packet 10 → 4.0 mm, packet 12 → 4.8 mm → Δ = 0.8 mm in two cycles.
 *   A reset via downlink 0x01 clears the counter to 0.
 */

// ── CayenneLPP Type Constants ──────────────────────────────────────────────
var LPP_DIGITAL_INPUT    = 0;   // 1 Byte unsigned
var LPP_DIGITAL_OUTPUT   = 1;   // 1 Byte unsigned
var LPP_PERCENTAGE       = 120; // 1 Byte unsigned
var LPP_POWER            = 128; // 2 Byte unsigned, W
var LPP_ENERGY           = 131; // 4 Byte unsigned, ÷1000 (kWh)
var LPP_ANALOG_INPUT     = 2;   // 2 Byte signed, ÷100
var LPP_TEMPERATURE      = 103; // 2 Byte signed, ÷10
var LPP_HUMIDITY         = 104; // 1 Byte unsigned, ÷2
var LPP_BAROMETRIC       = 115; // 2 Byte unsigned, ÷10
var LPP_VOLTAGE          = 116; // 2 Byte unsigned, ÷100
var LPP_DIRECTION        = 132; // 2 Byte unsigned (Grad)
var LPP_DISTANCE         = 130; // 4 Byte unsigned, ÷1000 (firmware maps result to mm/in)
var LPP_GENERIC_SENSOR   = 100; // 4 Byte unsigned, ×1

// ── Helper Functions ────────────────────────────────────────────────────────

function uint16BE(bytes, i) {
    return (bytes[i] << 8) | bytes[i + 1];
}

function int16BE(bytes, i) {
    var v = uint16BE(bytes, i);
    return v >= 0x8000 ? v - 0x10000 : v;
}

function roundTo(v, decimals) {
    var f = Math.pow(10, decimals);
    return Math.round(v * f) / f;
}

function uint32BE(bytes, i) {
    return ((bytes[i] << 24) | (bytes[i+1] << 16) | (bytes[i+2] << 8) | bytes[i+3]) >>> 0;
}

// Bit 6 selects Imperial units (°F, in, inHg, mph) for ALL temperature/
// rain/pressure/wind fields in this uplink — a single node-wide setting, not
// per-field. Values in the payload are ALREADY in the indicated unit (the
// firmware converts before encoding); this function only labels them.
function decodeStatus(byte) {
    var imperial = (byte & 0x40) !== 0;
    return {
        bme280_present:   (byte & 0x01) !== 0,
        bus1_has_sensors: (byte & 0x02) !== 0,
        send_fail_count:  (byte >> 2) & 0x0F,
        wind_speed_unit:  imperial ? "mph" : ((byte & 0x80) !== 0 ? "m/s" : "km/h"),
        temperature_unit: imperial ? "F" : "C",
        rain_unit:        imperial ? "in" : "mm",
        rain_rate_unit:   imperial ? "in/h" : "mm/h",
        pressure_unit:    imperial ? "inHg" : "hPa"
    };
}

// io status (PCB v2.0, LPP Ch 3 / binary IO_STATUS)
function decodeIoStatus(byte) {
    return {
        heater_on:           (byte & 0x01) !== 0,
        fan_on:              (byte & 0x02) !== 0,
        fan_stalled:         (byte & 0x04) !== 0,
        battery_low_lockout: (byte & 0x08) !== 0,
        vedirect_no_data:    (byte & 0x10) !== 0
    };
}

var VICTRON_CS = { 0: "off", 2: "fault", 3: "bulk", 4: "absorption", 5: "float", 7: "equalize",
                   245: "starting up", 247: "auto equalize", 252: "external control" };

function setChargeState(result, cs) {
    result.victron_charge_state = cs;
    if (VICTRON_CS[cs] !== undefined) result.victron_charge_state_text = VICTRON_CS[cs];
}

// ── Main Decoder ──────────────────────────────────────────────────────────

function parseLPP(bytes) {
    var result = {};
    var i = 0;

    // counters for sensor indices per type+channel
    var analogCount  = {};
    var tempCount    = {};
    var baroCount    = {};
    var distCount    = {};
    var digiCount    = {};

    while (i < bytes.length) {
        var channel = bytes[i++];
        var type    = bytes[i++];

        switch (type) {

            case LPP_DIRECTION:  // wind direction
                if (channel === 1) result.wind_direction_deg = uint16BE(bytes, i);
                else if (channel === 2) result.wind2_direction_deg = uint16BE(bytes, i);
                i += 2;
                break;

            case LPP_ANALOG_INPUT:
                var aval = int16BE(bytes, i) / 100.0;
                i += 2;
                if (channel === 1) result.wind_speed_avg = aval;
                else if (channel === 2) result.wind_gust  = aval;
                else if (channel === 3) result.rain_rate_mmh  = aval;
                else if (channel === 5) result.aux_analog_v = aval;
                else if (channel === 6) result.wind2_speed_avg = aval;
                else if (channel === 7) result.wind2_gust = aval;
                else if (channel === 13) result.uv_index = aval;
                else if (channel === 30) result.victron_battery_a = aval;
                else if (channel === 31) result.victron_load_a = aval;
                else if (channel === 203) result.debug_free_heap_kib = aval;
                else if (channel === 204) result.debug_uptime_h = aval;
                break;

            case LPP_DISTANCE:
                // Firmware writes rain values directly in mm (or in) via
                // addDistance(); raw ÷1000 reconstructs the value. Rounded to
                // 3 decimals, not 1: a single tip in imperial is 0.008 in.
                var dval = roundTo(uint32BE(bytes, i) / 1000.0, 3);
                i += 4;
                if (channel === 1) result.rain_cycle_mm       = dval;
                else if (channel === 2) result.rain_since_start_mm = dval;
                break;

            case LPP_GENERIC_SENSOR:
                var gval = uint32BE(bytes, i);
                i += 4;
                if (channel === 203) result.debug_free_heap_kib = gval;
                else if (channel === 10) result.aux_pulses = gval;
                else if (channel === 11) result.aux_pulses_total = gval;
                else if (channel === 22) result.fan_rpm = gval;
                else if (channel === 204) result.debug_uptime_h = roundTo(gval / 10.0, 1);
                break;

            case LPP_TEMPERATURE:
                var tval = int16BE(bytes, i) / 10.0;
                i += 2;
                if (channel === 1) result.bme280_temperature_c = tval;
                else if (channel >= 2 && channel <= 4)
                    result["ds18b20_" + (channel - 2) + "_c"] = tval;
                else if (channel === 201) result.debug_cpu_temperature_c = tval;
                break;

            case LPP_DIGITAL_INPUT:
                var dival = bytes[i++];
                if (channel === 1) result.cycle_counter = dival; // legacy payloads
                else if (channel === 2) result.status = decodeStatus(dival);
                else if (channel === 200) result.debug_cycle_counter = dival;
                else if (channel === 202) result.debug_send_fail_count = dival;
                else if (channel === 3) result.io_status = decodeIoStatus(dival);
                else if (channel === 30) setChargeState(result, dival);
                else if (channel === 31) result.victron_error = dival;
                else if (channel === 14) {
                    result.uv_sensor_enabled = (dival & 0x01) !== 0;
                    result.uv_index_calibrated = (dival & 0x02) !== 0;
                }
                break;

            case LPP_DIGITAL_OUTPUT:
                var doval = bytes[i++];
                if (channel === 20) result.heater_duty_pct = doval;
                else if (channel === 21) result.fan_duty_pct = doval;
                break;

            case LPP_POWER:
                if (channel === 30) result.victron_pv_w = uint16BE(bytes, i);
                i += 2;
                break;

            case LPP_ENERGY:
                if (channel === 30) result.victron_yield_today_kwh = roundTo(uint32BE(bytes, i) / 1000.0, 3);
                i += 4;
                break;

            case LPP_PERCENTAGE:
                if (channel === 30) result.victron_soc_pct = bytes[i];
                i += 1;
                break;

            case LPP_VOLTAGE:
                var vval = uint16BE(bytes, i) / 100.0;
                i += 2;
                if (channel === 30) result.victron_battery_v = vval;
                else if (channel === 31) result.victron_pv_v = vval;
                else if (channel === 12) result.uv_sensor_voltage_v = vval;
                else result.battery_v = vval;
                break;

            case LPP_HUMIDITY:
                result.bme280_humidity_pct = bytes[i++] / 2.0;
                break;

            case LPP_BAROMETRIC:
                var pval = uint16BE(bytes, i) / 10.0;
                i += 2;
                if (channel === 1) result.bme280_pressure_qnh_hpa = pval;
                else if (channel === 2) result.bme280_pressure_abs_hpa = pval;
                break;

            default:
                // Unknown type – stop parsing to avoid incorrect offsets
                console.error("LPP: unknown type 0x" + type.toString(16) + " at offset " + (i-1));
                i = bytes.length;
                break;
        }
    }

    // Imperial: the barometric fields hold inHg x100 (see header comment).
    if (result.status && result.status.pressure_unit === "inHg") {
        if (result.bme280_pressure_qnh_hpa !== undefined)
            result.bme280_pressure_qnh_hpa = roundTo(result.bme280_pressure_qnh_hpa / 10.0, 2);
        if (result.bme280_pressure_abs_hpa !== undefined)
            result.bme280_pressure_abs_hpa = roundTo(result.bme280_pressure_abs_hpa / 10.0, 2);
    }
    if (result.uv_sensor_voltage_v !== undefined) result.uv_sensor_voltage_unit = "V";
    if (result.uv_index !== undefined) result.uv_index_unit = "index";
    return result;
}

// ── Custom Binary Decoder (FPort 3) ─────────────────────────────────────────
//
// Layout: [version:1][presence bitmask:4, little-endian][field data...]
// Only fields whose bit is set in the mask are present, in ascending
// bit-index order. Field values use the same scaling as CayenneLPP above,
// but big-endian and fixed-width (no per-field channel/type byte).
// Bit order is authoritative in firmware: enum BinField in src/payload.h.
//
// Version 2: in imperial mode (status Bit 6) rain amounts are x1000 and
// pressures x100; rain since start is uint32. Version 1 (older firmware):
// everything x10, rain since start uint16. Version 3 is v2 plus bit 31's
// five-byte UV extension; v1/v2 parsing remains unchanged.

var BIN_FIELD = {
    WIND_DIR: 0, WIND_SPEED: 1, WIND_GUST: 2,
    RAIN_RATE: 3, RAIN_MM: 4, RAIN_SINCE: 5,
    STATUS: 6, BATTERY: 7,
    BME_HUMIDITY: 8, BME_PRESS_QNH: 9, BME_PRESS_ABS: 10, BME_TEMP: 11,
    DS18B20_0: 12, DS18B20_1: 13, DS18B20_2: 14,
    DBG_CYCLE: 15, DBG_CPU_TEMP: 16, DBG_FAIL_COUNT: 17, DBG_FREE_HEAP: 18, DBG_UPTIME: 19,
    // PCB v2.0 I/O block
    IO_STATUS: 20, AUX_AIN: 21, AUX_PULSES: 22, AUX_TOTAL: 23, OUTPUT_DUTY: 24, FAN_RPM: 25, VE_DIRECT: 26, VE_LOAD: 27,
    WIND2_DIR: 28, WIND2_SPEED: 29, WIND2_GUST: 30, UV: 31
};

function uint32LE(bytes, i) {
    return ((bytes[i + 3] << 24) | (bytes[i + 2] << 16) | (bytes[i + 1] << 8) | bytes[i]) >>> 0;
}

function parseBin(bytes) {
    var result = {};
    var version = bytes[0];
    if (version !== 1 && version !== 2 && version !== 3) {
        return { error: "unsupported binary protocol version " + version + " (expected 1, 2 or 3)" };
    }
    if (bytes.length < 5) return { error: "truncated binary payload header" };
    result.proto_version = version;
    var mask = uint32LE(bytes, 1);
    var i    = 5;

    function has(bit) { return (mask & (1 << bit)) !== 0; }

    // Rain/pressure are read raw first: their scale depends on the status
    // byte (imperial bit), which comes later in the bit order.
    var rainCycleRaw, rainSinceRaw, pressQnhRaw, pressAbsRaw;

    if (has(BIN_FIELD.WIND_DIR))       { result.wind_direction_deg = uint16BE(bytes, i); i += 2; }
    if (has(BIN_FIELD.WIND_SPEED))     { result.wind_speed_avg     = int16BE(bytes, i) / 100.0; i += 2; }
    if (has(BIN_FIELD.WIND_GUST))      { result.wind_gust          = int16BE(bytes, i) / 100.0; i += 2; }
    if (has(BIN_FIELD.RAIN_RATE))      { result.rain_rate_mmh      = int16BE(bytes, i) / 100.0; i += 2; }
    if (has(BIN_FIELD.RAIN_MM))        { rainCycleRaw = uint16BE(bytes, i); i += 2; }
    if (has(BIN_FIELD.RAIN_SINCE)) {
        if (version >= 2) { rainSinceRaw = uint32BE(bytes, i); i += 4; }
        else              { rainSinceRaw = uint16BE(bytes, i); i += 2; }
    }
    if (has(BIN_FIELD.STATUS))         { result.status = decodeStatus(bytes[i]); i += 1; }
    if (has(BIN_FIELD.BATTERY))        { result.battery_v = uint16BE(bytes, i) / 100.0; i += 2; }
    if (has(BIN_FIELD.BME_HUMIDITY))   { result.bme280_humidity_pct = bytes[i] / 2.0; i += 1; }
    if (has(BIN_FIELD.BME_PRESS_QNH))  { pressQnhRaw = uint16BE(bytes, i); i += 2; }
    if (has(BIN_FIELD.BME_PRESS_ABS))  { pressAbsRaw = uint16BE(bytes, i); i += 2; }
    if (has(BIN_FIELD.BME_TEMP))       { result.bme280_temperature_c    = int16BE(bytes, i) / 10.0; i += 2; }
    if (has(BIN_FIELD.DS18B20_0))      { result.ds18b20_0_c = int16BE(bytes, i) / 10.0; i += 2; }
    if (has(BIN_FIELD.DS18B20_1))      { result.ds18b20_1_c = int16BE(bytes, i) / 10.0; i += 2; }
    if (has(BIN_FIELD.DS18B20_2))      { result.ds18b20_2_c = int16BE(bytes, i) / 10.0; i += 2; }
    if (has(BIN_FIELD.DBG_CYCLE))      { result.debug_cycle_counter     = bytes[i]; i += 1; }
    if (has(BIN_FIELD.DBG_CPU_TEMP))   { result.debug_cpu_temperature_c = int16BE(bytes, i) / 10.0; i += 2; }
    if (has(BIN_FIELD.DBG_FAIL_COUNT)) { result.debug_send_fail_count   = bytes[i]; i += 1; }
    if (has(BIN_FIELD.DBG_FREE_HEAP))  { result.debug_free_heap_kib     = uint16BE(bytes, i); i += 2; }
    if (has(BIN_FIELD.DBG_UPTIME))     { result.debug_uptime_h          = uint16BE(bytes, i) / 10.0; i += 2; }
    if (has(BIN_FIELD.IO_STATUS))      { result.io_status = decodeIoStatus(bytes[i]); i += 1; }
    if (has(BIN_FIELD.AUX_AIN))        { result.aux_analog_v = uint16BE(bytes, i) / 1000.0; i += 2; }
    if (has(BIN_FIELD.AUX_PULSES))     { result.aux_pulses = uint16BE(bytes, i); i += 2; }
    if (has(BIN_FIELD.AUX_TOTAL))      { result.aux_pulses_total = uint32BE(bytes, i); i += 4; }
    if (has(BIN_FIELD.OUTPUT_DUTY))    { result.heater_duty_pct = bytes[i]; result.fan_duty_pct = bytes[i + 1]; i += 2; }
    if (has(BIN_FIELD.FAN_RPM))        { result.fan_rpm = uint16BE(bytes, i); i += 2; }
    if (has(BIN_FIELD.VE_DIRECT)) {
        // Fields the device doesn't send are marked 0xFFFF / 0x8000 / 0xFF.
        var bv = uint16BE(bytes, i), ba = uint16BE(bytes, i + 2), pv = uint16BE(bytes, i + 4),
            pw = uint16BE(bytes, i + 6), cs = bytes[i + 8], er = bytes[i + 9],
            yd = uint16BE(bytes, i + 10), soc = uint16BE(bytes, i + 12);
        if (bv !== 0xFFFF) result.victron_battery_v = bv / 100.0;
        if (ba !== 0x8000) result.victron_battery_a = int16BE(bytes, i + 2) / 100.0;
        if (pv !== 0xFFFF) result.victron_pv_v = pv / 100.0;
        if (pw !== 0xFFFF) result.victron_pv_w = pw;
        if (cs !== 0xFF)   setChargeState(result, cs);
        if (er !== 0xFF)   result.victron_error = er;
        if (yd !== 0xFFFF) result.victron_yield_today_kwh = yd / 100.0;
        if (soc !== 0xFFFF) result.victron_soc_pct = soc / 10.0;
        i += 14;
    }
    if (has(BIN_FIELD.VE_LOAD))        { result.victron_load_a = int16BE(bytes, i) / 100.0; i += 2; }
    if (has(BIN_FIELD.WIND2_DIR))      { result.wind2_direction_deg = uint16BE(bytes, i); i += 2; }
    if (has(BIN_FIELD.WIND2_SPEED))    { result.wind2_speed_avg = int16BE(bytes, i) / 100.0; i += 2; }
    if (has(BIN_FIELD.WIND2_GUST))     { result.wind2_gust = int16BE(bytes, i) / 100.0; i += 2; }
    if (has(BIN_FIELD.UV)) {
        if (version < 3) return { error: "UV field requires binary protocol version 3" };
        if (bytes.length < i + 5) return { error: "truncated binary UV field" };
        var uvVoltsRaw = uint16BE(bytes, i); i += 2;
        var uvIndexRaw = uint16BE(bytes, i); i += 2;
        var uvFlags = bytes[i++];
        result.uv_sensor_enabled = (uvFlags & 0x01) !== 0;
        if (result.uv_sensor_enabled) result.uv_sensor_voltage_v = uvVoltsRaw / 1000.0;
        result.uv_index_calibrated = result.uv_sensor_enabled &&
            (uvFlags & 0x02) !== 0 && uvIndexRaw !== 0xFFFF;
        if (result.uv_index_calibrated) result.uv_index = uvIndexRaw / 100.0;
    }

    if (i > bytes.length) return { error: "truncated binary payload" };
    if (result.uv_sensor_voltage_v !== undefined) result.uv_sensor_voltage_unit = "V";
    if (result.uv_index !== undefined) result.uv_index_unit = "index";

    var imperialV2 = version >= 2 && result.status && result.status.rain_unit === "in";
    var rainDiv  = imperialV2 ? 1000.0 : 10.0;
    var pressDiv = imperialV2 ? 100.0 : 10.0;
    if (rainCycleRaw !== undefined) result.rain_cycle_mm       = roundTo(rainCycleRaw / rainDiv, 3);
    if (rainSinceRaw !== undefined) result.rain_since_start_mm = roundTo(rainSinceRaw / rainDiv, 3);
    if (pressQnhRaw !== undefined)  result.bme280_pressure_qnh_hpa = roundTo(pressQnhRaw / pressDiv, 2);
    if (pressAbsRaw !== undefined)  result.bme280_pressure_abs_hpa = roundTo(pressAbsRaw / pressDiv, 2);

    return result;
}

// ── ChirpStack v4 Entry Point ──────────────────────────────────────────────

function decodeUplink(input) {
    try {
        var data = (input.fPort === 3) ? parseBin(input.bytes) : parseLPP(input.bytes);
        return { data: data, errors: [] };
    } catch (e) {
        return { data: {}, errors: [e.toString()] };
    }
}

// ── ChirpStack v3 Entry Point ──────────────────────────────────────────────

function Decode(fPort, bytes) {
    try {
        return (fPort === 3) ? parseBin(bytes) : parseLPP(bytes);
    } catch (e) {
        return { error: e.toString() };
    }
}

)DECODERJS";
