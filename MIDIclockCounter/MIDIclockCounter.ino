
#include <SoftwareSerial.h>
#include <Tiny4kOLED.h>

// Pines ATtiny85
const byte MIDI_TX = 1;  // PB1, pin fisico 6
const byte ENC_CLK = 3;  // PB3, pin fisico 2
const byte ENC_DT  = 4;  // PB4, pin fisico 3

// MIDI OUT
SoftwareSerial midiOut(-1, MIDI_TX);

// BPM inicial: 80,0
int bpm10 = 800;

const int BPM_MIN10 = 400;
const int BPM_MAX10 = 2400;

// Encoder
byte oldEncoderState = 0;
int encoderSteps = 0;
unsigned long lastTurn = 0;
bool firstTurn = true;

// MIDI Clock
unsigned long nextClock = 0;

// Tabla de decodificacion
const int8_t encoderTable[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

// Intervalo MIDI: 24 pulsos por negra
unsigned long clockInterval() {
  return (600000000UL + 12UL * bpm10) /
         (24UL * bpm10);
}

// Pantalla OLED
void updateDisplay() {
  oled.clear();
  oled.setFontX2(FONT8X16);
  oled.setCursor(1, 0);

  if (bpm10 < 1000) {
    oled.print(" ");
  }

  oled.print(bpm10 / 10);
  oled.print(".");
  oled.print(bpm10 % 10);
}

// Cambiar BPM
void changeBPM(int amount) {
  int oldBpm = bpm10;

  bpm10 += amount;
  bpm10 = constrain(bpm10, BPM_MIN10, BPM_MAX10);

  if (bpm10 != oldBpm) {
    updateDisplay();
  }
}

// Aceleracion del encoder
int getAcceleration(unsigned long elapsed) {

  if (elapsed < 200) {
    return 50;  // Muy rapido: 5 BPM
  }

  if (elapsed < 440) {
    return 20;  // Rapido: 2 BPM
  }

  if (elapsed < 1000) {
    return 10;  // Normal: 1 BPM
  }

  return 1;     // Lento: 0,1 BPM
}

// Leer encoder
void readEncoder() {

  byte clk = digitalRead(ENC_CLK);
  byte dt  = digitalRead(ENC_DT);

  byte newState = (clk << 1) | dt;

  if (newState != oldEncoderState) {

    byte index = (oldEncoderState << 2) | newState;

    encoderSteps += encoderTable[index];

    oldEncoderState = newState;

    if (encoderSteps >= 4 || encoderSteps <= -4) {

      unsigned long now = millis();

      int amount = 1;

      if (!firstTurn) {
        amount = getAcceleration(now - lastTurn);
      }

      // Sentido de giro invertido
      if (encoderSteps >= 4) {
        changeBPM(-amount);
      } else {
        changeBPM(amount);
      }

      encoderSteps = 0;
      lastTurn = now;
      firstTurn = false;
    }
  }
}

void setup() {

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);

  oldEncoderState =
      (digitalRead(ENC_CLK) << 1) |
       digitalRead(ENC_DT);

  midiOut.begin(31250);

  oled.begin();
  oled.clear();
  oled.on();

  updateDisplay();

  nextClock = micros();
}

void loop() {

  // Ajustar BPM con encoder
  readEncoder();

  // Enviar MIDI Clock continuamente
  unsigned long nowUs = micros();

  if ((int32_t)(nowUs - nextClock) >= 0) {

    midiOut.write((uint8_t)0xF8);

    unsigned long interval = clockInterval();

    nextClock += interval;

    if ((int32_t)(nowUs - nextClock) >=
        (int32_t)interval) {

      nextClock = nowUs + interval;
    }
  }
}
