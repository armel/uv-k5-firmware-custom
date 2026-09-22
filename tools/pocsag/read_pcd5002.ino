/* Dump a Philips PCD5002 paging decoder's EEPROM over I2C.
 *
 * Use this rather than a CH341A. A CH341A reads the pager's 24C32 message
 * EEPROM perfectly but cannot read this chip: bytes come back intermittently
 * shifted one bit, and the address pointer reads back shifted too. The
 * difference is clock stretching. The 24C32 never stretches SCL; the PCD5002
 * runs from a 76.8 kHz crystal and does, and the CH341A's I2C engine does not
 * wait for it. Its slowest setting, 20 kHz, is still far too fast to paper over
 * that.
 *
 * AVR TWI hardware (Uno, Nano, Pro Mini) honours clock stretching natively, as
 * do the ESP32 and STM32 peripherals, which is the whole reason to use one here.
 *
 * VOLTAGE: the PCD5002's absolute maximum on SDA and SCL is VPR + 0.8V, around
 * 3.5V for a typical 2.7V pager rail. A 5V board will damage it. Use a 3.3V
 * board: Pro Mini 3.3V, ESP32, STM32 "blue pill", Due, or a level shifter.
 *
 * WIRING
 *   PCD5002 pin 9  SDA  -> board SDA
 *   PCD5002 pin 10 SCL  -> board SCL
 *   PCD5002 pin 12 VSS  -> board GND        (common ground is essential)
 *
 * PULL-UPS: try none of your own first. The pager board already pulls this bus
 * up to its own rail, because its controller, the decoder and the 24C32 all
 * share it. Adding pull-ups to the board's 3V3 instead sets two supplies
 * against each other and can exceed the chip's SDA/SCL maximum of VPR + 0.8 V.
 * If edges are genuinely too slow, add 2k2 to the PAGER's rail, not the
 * board's, and slow Wire.setClock() further.
 *
 * Leave the pager on its own battery and switched OFF. Reading cannot corrupt
 * anything: writes need a programming enable bit this sketch never sets.
 *
 * Paste the hex it prints into a file and decode with tools/pocsag/pcd5002.py.
 */

#include <Wire.h>

static const uint8_t PCD5002_ADDR    = 0x27;  // 7-bit
static const uint8_t IDX_CONTROL     = 0x00;  // write: Table 18
static const uint8_t IDX_EEPROM_PTR  = 0x07;  // bits 5:3 row, 2:0 column
static const uint8_t IDX_EEPROM_DATA = 0x0A;
static const uint8_t EEPROM_BYTES    = 48;    // 8 rows x 6 columns

// Each byte is read RUN times and accepted only if all RUN agree. There is
// deliberately no retry budget: at the corruption rate a CH341A shows here,
// roughly an even split between a value and its one-bit shift, extra retries
// only give a wrong unanimous run more chances to appear. Simulation put a
// majority vote at 70 wrong answers in 400 and a retried run at 113; a single
// unanimous window is the only shape that stays honest, and it fails loudly
// so the link gets fixed instead of guessed around.
static const uint8_t RUN = 6;                 // unanimous reads required
static const uint8_t PACE_MS = 5;             // the part is slow; let it settle
static const uint8_t LINK_SAMPLES = 20;       // reads used to judge the link

static uint8_t eepromAddress(uint8_t i)
{
    // The matrix has 6 columns, so address = (row << 3) | column and the
    // datasheet addresses are non-contiguous.
    return ((i / 6) << 3) | (i % 6);
}

// Message form (a): S ADDR+W INDEX [DATA] P
static bool selectIndex(uint8_t index, bool withData, uint8_t data)
{
    Wire.beginTransmission(PCD5002_ADDR);
    Wire.write(index);
    if (withData)
        Wire.write(data);
    return Wire.endTransmission() == 0;
}

// Message form (b): S ADDR+R DATA.. P, a message of its own. The index it draws
// from is whichever was written last.
static bool readByte(uint8_t *out)
{
    if (Wire.requestFrom(PCD5002_ADDR, (uint8_t)1) != 1)
        return false;
    *out = Wire.read();
    return true;
}

static bool readOnce(uint8_t addr, uint8_t *out)
{
    delay(PACE_MS);
    if (!selectIndex(IDX_EEPROM_PTR, true, addr))
        return false;

    delay(PACE_MS);
    if (!selectIndex(IDX_EEPROM_DATA, false, 0))
        return false;

    delay(PACE_MS);
    return readByte(out);
}

static bool readAddress(uint8_t addr, uint8_t *out)
{
    uint8_t first;

    if (!readOnce(addr, &first))
        return false;

    for (uint8_t i = 1; i < RUN; i++) {
        uint8_t v;
        if (!readOnce(addr, &v) || v != first)
            return false;
    }

    *out = first;
    return true;
}

// Judge the link before trusting a single byte of it. One address, read many
// times: on a sound link every read matches, and anything else means the dump
// would be fiction.
static bool assessLink(uint8_t addr)
{
    uint8_t values[LINK_SAMPLES];
    uint8_t n = 0, distinct = 0;

    for (uint8_t i = 0; i < LINK_SAMPLES; i++)
        if (readOnce(addr, &values[n])) n++;

    for (uint8_t i = 0; i < n; i++) {
        bool seenBefore = false;
        for (uint8_t j = 0; j < i; j++)
            if (values[j] == values[i]) seenBefore = true;
        if (!seenBefore) distinct++;
    }

    Serial.print(F("link check at 0x"));
    if (addr < 0x10) Serial.print('0');
    Serial.print(addr, HEX);
    Serial.print(F(": "));
    Serial.print(n);
    Serial.print(F("/"));
    Serial.print(LINK_SAMPLES);
    Serial.print(F(" reads, "));
    Serial.print(distinct);
    Serial.println(F(" distinct value(s)"));

    Serial.print(F("  saw:"));
    for (uint8_t i = 0; i < n; i++) {
        Serial.print(' ');
        if (values[i] < 0x10) Serial.print('0');
        Serial.print(values[i], HEX);
    }
    Serial.println();

    if (n == 0) {
        Serial.println(F("  no reads succeeded at all"));
        return false;
    }
    if (distinct == 1) {
        Serial.println(F("  stable: the link is sound, dumping"));
        return true;
    }

    Serial.println(F("  UNSTABLE. Reading one address must give one answer, so"));
    Serial.println(F("  a dump now would be fiction. No amount of voting fixes"));
    Serial.println(F("  this; fix the link:"));
    Serial.println(F("    - shorter leads, this matters most"));
    Serial.println(F("    - 2k2 pull-ups to the pager's own VDD, not the board's"));
    Serial.println(F("    - a short ground return right next to the data pair"));
    Serial.println(F("    - confirm the board is 3.3V, not 5V"));
    return false;
}

void setup()
{
    Serial.begin(115200);
    while (!Serial) { }

    Wire.begin();
    Wire.setClock(20000);         // slow, to suit a 76.8 kHz part

    Serial.println(F("PCD5002 EEPROM dump"));

    Wire.beginTransmission(PCD5002_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println(F("no ACK from 0x27 -- check wiring, pull-ups, and that"));
        Serial.println(F("the pager is powered and switched off"));
        return;
    }

    // Table 18: D4 = 0 selects OFF status (while DON is low), D3 = 0 leaves the
    // receiver alone, D1 = 0 keeps EEPROM writes disabled so this cannot alter
    // anything. 8.50 wants the receiver inactive before EEPROM access.
    selectIndex(IDX_CONTROL, true, 0x00);
    delay(50);

    if (!assessLink(0x10))
        return;

    uint8_t data[EEPROM_BYTES];
    uint8_t failures = 0;

    for (uint8_t i = 0; i < EEPROM_BYTES; i++)
        if (!readAddress(eepromAddress(i), &data[i]))
            failures++;

    if (failures) {
        Serial.print(F("FAILED: "));
        Serial.print(failures);
        Serial.print(F(" of "));
        Serial.print(EEPROM_BYTES);
        Serial.println(F(" bytes did not give identical reads."));
        Serial.println(F("The link passed the initial check but is not good enough."));
        return;
    }

    // Row-major, 6 bytes per row, labelled with the datasheet address.
    for (uint8_t r = 0; r < 8; r++) {
        Serial.print(F("  "));
        if ((r << 3) < 0x10) Serial.print('0');
        Serial.print(r << 3, HEX);
        Serial.print(F("  "));
        for (uint8_t c = 0; c < 6; c++) {
            const uint8_t b = data[r * 6 + c];
            if (b < 0x10) Serial.print('0');
            Serial.print(b, HEX);
            Serial.print(' ');
        }
        Serial.println();
    }

    Serial.println(F("done -- save these 48 bytes and run pcd5002.py on them"));
}

void loop() { }
