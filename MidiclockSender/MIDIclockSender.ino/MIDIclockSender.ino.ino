// MIDI Clock para Arduino Nano
// - Encoder: cambia el BPM (con aceleracion)
// - Boton (pulsacion corta): Start / Stop  (envia 0xFA / 0xFC)
// - Boton (mantener 0,7 s): modo TAP TEMPO (el numero se ve invertido)
//       pulsa al ritmo deseado; sale solo tras 3 s sin pulsar
// - Pantalla: solo el BPM, en grande
//
// Conexiones:
//   OLED:    SDA -> A4, SCL -> A5, VCC -> 5V (o 3V3), GND -> GND
//   Encoder: SW -> D2, CLK -> D3, DT -> D4, GND -> GND
//   MIDI OUT (DIN): D1 (TX) -> 220 ohm -> pin 5 del DIN
//                   5V -> 220 ohm -> pin 4 del DIN
//                   GND -> pin 2 del DIN
//   IMPORTANTE: desconecta el cable DIN de D1 al subir el sketch.
//
// Libreria: SSD1306Ascii (Bill Greiman)
// Placa: Arduino Nano, Procesador: ATmega328P (Old Bootloader)

#include <Wire.h>
#include <EEPROM.h>
#include "SSD1306Ascii.h"
#include "SSD1306AsciiWire.h"

#define OLED_ADDR 0x3C

// Pon aqui el MISMO valor que te funciono en el sketch de prueba:
//   0 = SSD1306 128x64,  1 = SH1106 128x64,  2 = SSD1306 128x32
#define OLED_TYPE 0

// Fuente grande de numeros. Si no se ve el punto decimal o los digitos,
// cambia a: #define BPM_FONT Adafruit5x7  y  #define GLYPH_W 12
// (y BPM_ROW a 3 en pantallas de 64 px de alto)
#define BPM_FONT lcdnums12x16
#define GLYPH_W  24            // ancho de cada caracter en pixeles (con 2X)

#if OLED_TYPE == 2
  #define BPM_ROW 0
#else
  #define BPM_ROW 2
#endif

// 1 = envia el clock siempre (el equipo ya sigue el tempo antes del Start)
// 0 = envia el clock solo mientras esta en marcha
#define CLOCK_ALWAYS 1

SSD1306AsciiWire oled;

const byte ENC_SW  = 2;
const byte ENC_CLK = 3;
const byte ENC_DT  = 4;

// BPM en decimas (800 = 80.0)
int bpm10 = 1200;
const int BPM_MIN10 = 400;
const int BPM_MAX10 = 2400;

volatile bool running = false;

// Guardado del ultimo BPM en la EEPROM
// (se guarda 2 s despues del ultimo cambio, para no gastar la memoria)
const byte EEPROM_MAGIC = 0xA5;
const int EEPROM_ADDR_MAGIC = 0;
const int EEPROM_ADDR_BPM = 1;
const unsigned long SAVE_DELAY_MS = 2000;
bool needSave = false;
unsigned long lastBpmChange = 0;

// ---------- Encoder (por interrupcion, no pierde pasos) ----------
const int8_t encoderTable[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

volatile byte encState = 0;
volatile int8_t encSteps = 0;
volatile int encPending = 0;
volatile unsigned long encLastMs = 0;
volatile bool encFirst = true;

int getAcceleration(unsigned long elapsed) {
  if (elapsed < 200) return 5;     // rapido: 0.5 BPM
  if (elapsed < 440) return 2;     // normal: 0.2 BPM
  return 1;                        // lento:  0.1 BPM
}

ISR(PCINT2_vect) {
  byte clk = (PIND >> 3) & 1;      // D3
  byte dt  = (PIND >> 4) & 1;      // D4
  byte newState = (clk << 1) | dt;

  if (newState == encState) return;

  encSteps += encoderTable[(encState << 2) | newState];
  encState = newState;

  if (encSteps >= 4 || encSteps <= -4) {
    unsigned long now = millis();
    int amount = 1;
    if (!encFirst) amount = getAcceleration(now - encLastMs);

    // Sentido de giro igual que en tu programa del ATtiny
    encPending += (encSteps >= 4) ? -amount : amount;

    encSteps = 0;
    encLastMs = now;
    encFirst = false;
  }
}

// ---------- MIDI clock (Timer1, 24 pulsos por negra) ----------
ISR(TIMER1_COMPA_vect) {
#if CLOCK_ALWAYS
  Serial.write((uint8_t)0xF8);
#else
  if (running) Serial.write((uint8_t)0xF8);
#endif
}

void applyTempo() {
  unsigned long ocr;
  byte cs;

  if (bpm10 >= 800) {
    // prescaler 8: 0,5 us por tick
    ocr = (50000000UL + bpm10 / 2) / bpm10 - 1;
    cs = _BV(CS11);
  } else {
    // prescaler 64: 4 us por tick (tempos lentos)
    ocr = (6250000UL + bpm10 / 2) / bpm10 - 1;
    cs = _BV(CS11) | _BV(CS10);
  }

  noInterrupts();
  OCR1A = (uint16_t)ocr;
  TCCR1B = _BV(WGM12) | cs;        // modo CTC
  if (TCNT1 >= (uint16_t)ocr) TCNT1 = 0;
  interrupts();
}

// ---------- Pantalla ----------
bool tapMode = false;
bool dirty = true;
bool forceRedraw = true;
char shown[8] = {0};
byte shownLen = 0;

void drawBpm() {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d.%d", bpm10 / 10, bpm10 % 10);
  byte len = strlen(buf);

  if (len != shownLen) {
    oled.clear();
    shownLen = len;
    memset(shown, 0, sizeof(shown));
  }
  if (forceRedraw) {
    memset(shown, 0, sizeof(shown));
    forceRedraw = false;
  }

  oled.setInvertMode(tapMode);

  byte col0 = (128 - len * GLYPH_W) / 2;
  for (byte i = 0; i < len; i++) {
    if (buf[i] != shown[i]) {
      oled.setCursor(col0 + i * GLYPH_W, BPM_ROW);
      oled.print(buf[i]);
      shown[i] = buf[i];
    }
  }
}

void setBpm(int value) {
  value = constrain(value, BPM_MIN10, BPM_MAX10);
  if (value != bpm10) {
    bpm10 = value;
    applyTempo();
    dirty = true;
    needSave = true;
    lastBpmChange = millis();
  }
}

// ---------- Start / Stop ----------
void toggleRun() {
  noInterrupts();
  if (!running) {
    TCNT1 = 0;                      // el primer 0xF8 llega un periodo despues
    Serial.write((uint8_t)0xFA);    // Start
    running = true;
  } else {
    running = false;
    Serial.write((uint8_t)0xFC);    // Stop
  }
  interrupts();
}

// ---------- Tap tempo ----------
const byte TAPS = 8;
const unsigned long TAP_GAP_RESET = 2000;   // pausa que reinicia la serie
const unsigned long TAP_TIMEOUT   = 3000;   // sale del modo tap
unsigned long tapTimes[TAPS];
byte tapCount = 0;
unsigned long lastTapActivity = 0;

void enterTapMode(unsigned long now) {
  tapMode = true;
  tapCount = 0;
  lastTapActivity = now;
  forceRedraw = true;
  dirty = true;
}

void registerTap(unsigned long now) {
  if (tapCount > 0 && now - tapTimes[tapCount - 1] > TAP_GAP_RESET) {
    tapCount = 0;
  }
  if (tapCount == TAPS) {
    for (byte i = 0; i < TAPS - 1; i++) tapTimes[i] = tapTimes[i + 1];
    tapCount = TAPS - 1;
  }
  tapTimes[tapCount++] = now;
  lastTapActivity = now;

  if (tapCount >= 2) {
    unsigned long span = tapTimes[tapCount - 1] - tapTimes[0];
    unsigned long n = tapCount - 1;
    if (span > 0) {
      setBpm((int)((600000UL * n + span / 2) / span));
    }
  }
}

// ---------- Boton ----------
const unsigned long LONG_PRESS_MS = 700;
bool btnDown = false;
bool longDone = false;
bool pressedInTap = false;
unsigned long btnChange = 0;
unsigned long pressStart = 0;

void readButton() {
  unsigned long now = millis();
  bool down = (digitalRead(ENC_SW) == LOW);

  if (down != btnDown && now - btnChange > 25) {
    btnDown = down;
    btnChange = now;

    if (down) {
      pressStart = now;
      longDone = false;
      pressedInTap = tapMode;
      if (tapMode) registerTap(now);          // en modo tap, cada pulso es un tap
    } else {
      if (!pressedInTap && !longDone) toggleRun();   // pulsacion corta
    }
  }

  // Pulsacion larga: entra en modo tap
  if (btnDown && !longDone && !pressedInTap && now - pressStart >= LONG_PRESS_MS) {
    longDone = true;
    enterTapMode(now);
  }

  // Salida automatica del modo tap
  if (tapMode && now - lastTapActivity > TAP_TIMEOUT) {
    tapMode = false;
    tapCount = 0;
    forceRedraw = true;
    dirty = true;
  }
}

// ---------- Setup / loop ----------
void setup() {
  Serial.begin(31250);               // MIDI

  pinMode(ENC_SW,  INPUT_PULLUP);
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);

  encState = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);

  // Recuperar el ultimo BPM guardado
  if (EEPROM.read(EEPROM_ADDR_MAGIC) == EEPROM_MAGIC) {
    int saved;
    EEPROM.get(EEPROM_ADDR_BPM, saved);
    if (saved >= BPM_MIN10 && saved <= BPM_MAX10) bpm10 = saved;
  }

  delay(300);                        // la pantalla necesita arrancar

  Wire.begin();                      // bus a 100 kHz

#if OLED_TYPE == 0
  oled.begin(&Adafruit128x64, OLED_ADDR);
#elif OLED_TYPE == 1
  oled.begin(&SH1106_128x64, OLED_ADDR);
#else
  oled.begin(&Adafruit128x32, OLED_ADDR);
#endif

  oled.setFont(BPM_FONT);
  oled.set2X();
  oled.clear();

  // Interrupciones del encoder (D3 y D4)
  PCICR  |= _BV(PCIE2);
  PCMSK2 |= _BV(PCINT19) | _BV(PCINT20);

  // Timer1 para el MIDI clock
  TCCR1A = 0;
  TCNT1 = 0;
  TIMSK1 = _BV(OCIE1A);
  applyTempo();

  drawBpm();
}

void loop() {
  // Cambios del encoder
  int pending;
  noInterrupts();
  pending = encPending;
  encPending = 0;
  interrupts();

  if (pending != 0) setBpm(bpm10 + pending);

  readButton();

  // Guardar el BPM cuando lleva 2 s sin cambiar
  if (needSave && millis() - lastBpmChange > SAVE_DELAY_MS) {
    needSave = false;
    EEPROM.update(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
    EEPROM.put(EEPROM_ADDR_BPM, bpm10);   // put solo escribe si el valor cambia
  }

  if (dirty) {
    dirty = false;
    drawBpm();
  }
}
