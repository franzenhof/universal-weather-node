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
 *   Ch 1  Rel. Humidity    → bme280 humidity (%; the selected temperature/humidity source:
 *                            BME280 or a wireless sensor - see Ch 16 / "sources")
 *   Ch 1  Baro. Pressure   → bme280 barometric pressure QNH (hPa; inHg x10, see below)
 *   Ch 2  Baro. Pressure   → bme280 barometric pressure absolute (hPa; inHg x10, see below)
 *   Ch 1  Temperature      → bme280 temperature (°C; selected source like humidity; pressure
 *                            is always the BME280)
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
 *   Ch 14 Digital Input    → UV flags (bit 0 board UV voltage, bit 1 UV index: calibrated board
 *                            index or a wireless sensor)
 *   Ch 15 Generic Sensor   → illuminance (lux, wireless sensor)
 *   Ch 16 Generic Sensor   → value sources (see decodeSources), with wireless sensors on
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

// Data bytes per CayenneLPP type (after the channel and type bytes).
var LPP_SIZE = {};
LPP_SIZE[LPP_DIGITAL_INPUT] = 1; LPP_SIZE[LPP_DIGITAL_OUTPUT] = 1; LPP_SIZE[LPP_PERCENTAGE] = 1;
LPP_SIZE[LPP_HUMIDITY] = 1;      LPP_SIZE[LPP_POWER] = 2;          LPP_SIZE[LPP_ANALOG_INPUT] = 2;
LPP_SIZE[LPP_TEMPERATURE] = 2;   LPP_SIZE[LPP_BAROMETRIC] = 2;     LPP_SIZE[LPP_VOLTAGE] = 2;
LPP_SIZE[LPP_DIRECTION] = 2;     LPP_SIZE[LPP_ENERGY] = 4;         LPP_SIZE[LPP_DISTANCE] = 4;
LPP_SIZE[LPP_GENERIC_SENSOR] = 4;

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
        // A truncated field would read past the end (undefined -> NaN/0):
        // reject the payload instead of returning made-up values.
        if (i + 2 > bytes.length)
            return { error: "truncated CayenneLPP payload at offset " + i };
        var channel = bytes[i++];
        var type    = bytes[i++];
        if (LPP_SIZE[type] !== undefined && i + LPP_SIZE[type] > bytes.length)
            return { error: "truncated CayenneLPP field (channel " + channel + ", type " + type + ") at offset " + (i - 2) };

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
                else if (channel === 15) result.light_lux = gval;
                else if (channel === 16) decodeSources(result, gval);
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
                    result.uv_index_calibrated = (dival & 0x02) !== 0;   // calibrated board index or wireless sensor
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

// Value sources (firmware sources.h): 2 bits per group, 0 none, 1 board,
// 2 wireless sensor (pressure: bits 10-11, since the pressure source was
// added - older firmware leaves them 0 = "none"); bit 15 = a wireless
// sensor in use reports a low battery.
function decodeSources(result, v) {
    var names = ["temp_humidity", "wind", "rain", "uv", "light", "pressure"];
    var kinds = ["none", "board", "wireless", "unknown"];
    result.sources = {};
    for (var g = 0; g < names.length; g++) result.sources[names[g]] = kinds[(v >> (2 * g)) & 3];
    result.wireless_battery_low = (v & 0x8000) !== 0;
}

// ── Custom Binary Decoder (FPort 3) ─────────────────────────────────────────
//
// Layout: [version:1][presence bitmask:4, little-endian][field data...]
// Only fields whose bit is set in the mask are present, in ascending
// bit-index order. Field values use the same scaling as CayenneLPP above,
// but big-endian and fixed-width (no per-field channel/type byte).
// Bit order is authoritative in firmware: enum BinField in src/payload.h.
//
// Version 4 (current firmware): the mask has a variable length - each byte
// carries 7 field bits (bit 0-6 = fields 7k..7k+6), bit 7 = another mask
// byte follows. Bits 0-25 and 27-30 are as in v2/v3. Changed/added in v4:
// bit 26 Victron battery V only (u16 x100), 31 UV voltage (u16 x1000),
// 32 UV index (u16 x100), 33 value sources (u16, see decodeSources),
// 34 illuminance (u32 lux), 35-41 Victron battery A (i16 x100), PV V (u16
// x100), PV W (u16), charge state (u8), error (u8), yield today (u16 x100
// kWh), SOC (u16 x10 %). A value that is not available is simply absent.
//
// Older firmware - 4-byte little-endian mask:
// Version 2: in imperial mode (status Bit 6) rain amounts are x1000 and
// pressures x100; rain since start is uint32. Version 1: everything x10,
// rain since start uint16. Version 3 is v2 plus bit 31's five-byte UV group;
// bit 26 is the 14-byte VE.Direct block in v1-v3.

var BIN_FIELD = {
    WIND_DIR: 0, WIND_SPEED: 1, WIND_GUST: 2,
    RAIN_RATE: 3, RAIN_MM: 4, RAIN_SINCE: 5,
    STATUS: 6, BATTERY: 7,
    BME_HUMIDITY: 8, BME_PRESS_QNH: 9, BME_PRESS_ABS: 10, BME_TEMP: 11,
    DS18B20_0: 12, DS18B20_1: 13, DS18B20_2: 14,
    DBG_CYCLE: 15, DBG_CPU_TEMP: 16, DBG_FAIL_COUNT: 17, DBG_FREE_HEAP: 18, DBG_UPTIME: 19,
    // PCB v2.0 I/O block
    IO_STATUS: 20, AUX_AIN: 21, AUX_PULSES: 22, AUX_TOTAL: 23, OUTPUT_DUTY: 24, FAN_RPM: 25, VE_DIRECT: 26, VE_LOAD: 27,
    WIND2_DIR: 28, WIND2_SPEED: 29, WIND2_GUST: 30, UV: 31,
    // version 4
    UV_INDEX: 32, SOURCES: 33, LIGHT: 34, VE_BATT_A: 35, VE_PV_V: 36, VE_PV_W: 37,
    VE_CS: 38, VE_ERR: 39, VE_YIELD: 40, VE_SOC: 41
};

function uint32LE(bytes, i) {
    return ((bytes[i + 3] << 24) | (bytes[i + 2] << 16) | (bytes[i + 1] << 8) | bytes[i]) >>> 0;
}

function parseBin(bytes) {
    var result = {};
    var version = bytes[0];
    if (version < 1 || version > 4) {
        return { error: "unsupported binary protocol version " + version + " (expected 1 to 4)" };
    }
    result.proto_version = version;
    // presence bits as an array (more than 32 bits in v4)
    var bits = [];
    var i;
    if (version >= 4) {
        i = 1;
        var more = true;
        while (more) {
            if (i >= bytes.length) return { error: "truncated binary payload header" };
            var mb = bytes[i++];
            for (var k = 0; k < 7; k++) bits.push((mb >> k) & 1);
            more = (mb & 0x80) !== 0;
        }
    } else {
        if (bytes.length < 5) return { error: "truncated binary payload header" };
        var mask = uint32LE(bytes, 1);
        for (var b = 0; b < 32; b++) bits.push(b === 31 ? (mask >>> 31) & 1 : (mask >> b) & 1);
        i = 5;
    }
    var v4 = version >= 4;

    function has(bit) { return bit < bits.length && bits[bit] === 1; }

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
    if (v4 && has(BIN_FIELD.VE_DIRECT)) { result.victron_battery_v = uint16BE(bytes, i) / 100.0; i += 2; }
    else if (has(BIN_FIELD.VE_DIRECT)) {
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
    if (v4) {
        if (has(BIN_FIELD.UV))         { result.uv_sensor_voltage_v = uint16BE(bytes, i) / 1000.0; i += 2; }
        if (has(BIN_FIELD.UV_INDEX))   { result.uv_index = uint16BE(bytes, i) / 100.0; i += 2; }
        if (has(BIN_FIELD.UV) || has(BIN_FIELD.UV_INDEX)) {
            result.uv_sensor_enabled = has(BIN_FIELD.UV);            // board UV voltage
            result.uv_index_calibrated = has(BIN_FIELD.UV_INDEX);    // calibrated board index or wireless sensor
        }
        if (has(BIN_FIELD.SOURCES))    { decodeSources(result, uint16BE(bytes, i)); i += 2; }
        if (has(BIN_FIELD.LIGHT))      { result.light_lux = uint32BE(bytes, i); i += 4; }
        if (has(BIN_FIELD.VE_BATT_A))  { result.victron_battery_a = int16BE(bytes, i) / 100.0; i += 2; }
        if (has(BIN_FIELD.VE_PV_V))    { result.victron_pv_v = uint16BE(bytes, i) / 100.0; i += 2; }
        if (has(BIN_FIELD.VE_PV_W))    { result.victron_pv_w = uint16BE(bytes, i); i += 2; }
        if (has(BIN_FIELD.VE_CS))      { setChargeState(result, bytes[i]); i += 1; }
        if (has(BIN_FIELD.VE_ERR))     { result.victron_error = bytes[i]; i += 1; }
        if (has(BIN_FIELD.VE_YIELD))   { result.victron_yield_today_kwh = uint16BE(bytes, i) / 100.0; i += 2; }
        if (has(BIN_FIELD.VE_SOC))     { result.victron_soc_pct = uint16BE(bytes, i) / 10.0; i += 2; }
    } else if (has(BIN_FIELD.UV)) {
        if (version < 3) return { error: "UV field requires binary protocol version 3" };
        if (bytes.length < i + 5) return { error: "truncated binary UV field" };
        var uvVoltsRaw = uint16BE(bytes, i); i += 2;
        var uvIndexRaw = uint16BE(bytes, i); i += 2;
        var uvFlags = bytes[i++];
        result.uv_sensor_enabled = (uvFlags & 0x01) !== 0;            // board UV voltage
        if (result.uv_sensor_enabled && uvVoltsRaw !== 0xFFFF) result.uv_sensor_voltage_v = uvVoltsRaw / 1000.0;
        // bit 1: calibrated board index or a wireless sensor's index
        result.uv_index_calibrated = (uvFlags & 0x02) !== 0 && uvIndexRaw !== 0xFFFF;
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

// ── Derived values (same formulas as the firmware's src/derived.cpp) ──────
// Computed from the decoded values only - nothing extra is sent. Output in
// the payload's units (status.temperature_unit / wind_speed_unit).
function addDerived(r) {
    if (!r || r.error) return r;
    var st = r.status || {};
    var tf = st.temperature_unit === "F";
    var t = r.bme280_temperature_c, h = r.bme280_humidity_pct;
    var toC = function (v) { return tf ? (v - 32) / 1.8 : v; };
    var fromC = function (v) { return tf ? v * 1.8 + 32 : v; };
    if (typeof r.wind_direction_deg === "number") {
        var names = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"];
        r.wind_direction_text = names[Math.floor(((r.wind_direction_deg % 360 + 360) % 360 + 11.25) / 22.5) % 16];
    }
    if (typeof t !== "number") return r;
    var tc = toC(t);
    var windMs = null;
    if (typeof r.wind_speed_avg === "number") {
        var wu = st.wind_speed_unit;
        windMs = wu === "mph" ? r.wind_speed_avg * 0.44704 : wu === "m/s" ? r.wind_speed_avg : r.wind_speed_avg / 3.6;
    }
    var feels = tc;
    if (windMs !== null && tc <= 10 && windMs * 3.6 > 4.8) {
        var v = Math.pow(windMs * 3.6, 0.16);
        feels = Math.min(tc, 13.12 + 0.6215 * tc - 11.37 * v + 0.3965 * tc * v);
    } else if (typeof h === "number" && tc >= 26.7) {
        var f = tc * 1.8 + 32, hi = 0.5 * (f + 61 + (f - 68) * 1.2 + h * 0.094);
        if ((hi + f) / 2 >= 80) {
            hi = -42.379 + 2.04901523 * f + 10.14333127 * h - 0.22475541 * f * h - 0.00683783 * f * f
                 - 0.05481717 * h * h + 0.00122874 * f * f * h + 0.00085282 * f * h * h - 0.00000199 * f * f * h * h;
            if (h < 13 && f <= 112) hi -= (13 - h) / 4 * Math.sqrt((17 - Math.abs(f - 95)) / 17);
            else if (h > 85 && f <= 87) hi += (h - 85) / 10 * (87 - f) / 5;
        }
        feels = (hi - 32) / 1.8;
    }
    r.feels_like = roundTo(fromC(feels), 1);
    var warn = { frost: tc <= 0, fog: false, sultry: false };
    if (typeof h === "number") {
        var rh = Math.min(100, Math.max(1, h)), g = Math.log(rh / 100) + 17.62 * tc / (243.12 + tc);
        var td = 243.12 * g / (17.62 - g);
        r.dew_point = roundTo(fromC(td), 1);
        r.cloud_base_m = Math.round(Math.max(0, 125 * (tc - td)));
        warn.fog = tc - td <= 1 && h >= 95;
        warn.sultry = td >= 16;
    }
    r.warnings = warn;
    return r;
}

// ── ChirpStack v4 Entry Point ──────────────────────────────────────────────

function decodeUplink(input) {
    try {
        var data = addDerived((input.fPort === 3) ? parseBin(input.bytes) : parseLPP(input.bytes));
        // ChirpStack v4: a payload that can't be decoded is an error, not data
        if (data && typeof data.error === "string") return { data: {}, errors: [data.error] };
        return { data: data, errors: [] };
    } catch (e) {
        return { data: {}, errors: [e.toString()] };
    }
}

// ── ChirpStack v3 Entry Point ──────────────────────────────────────────────

function Decode(fPort, bytes) {
    try {
        return addDerived((fPort === 3) ? parseBin(bytes) : parseLPP(bytes));
    } catch (e) {
        return { error: e.toString() };
    }
}
