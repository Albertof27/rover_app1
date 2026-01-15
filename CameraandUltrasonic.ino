#include <Wire.h>
#include <SPI.h>
#include <ArduCAM.h>
#include "memorysaver.h"

// ------------------- Camera config (Mega) -------------------
#define OV2640_MINI_2MP
#define CS_PIN 53
ArduCAM cam(OV2640, CS_PIN);

// Drive ~8 MHz XCLK on Mega pin 11 using Timer1 (OC1A)
void enableXCLK() {
  pinMode(11, OUTPUT);
  // Toggle OC1A on compare match; CTC mode; no prescale
  TCCR1A = _BV(COM1A0);
  TCCR1B = _BV(WGM12) | _BV(CS10);
  OCR1A  = 1; // ~8 MHz; if unstable, try 3 or 7 for lower freq
}

bool waitCaptureDone(uint32_t ms = 3000) {
  uint32_t t0 = millis();
  while (!cam.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK)) {
    if (millis() - t0 > ms) return false;
  }
  return true;
}

void ov2640_force_jpeg() {
  cam.wrSensorReg8_8(0xFF, 0x00);
  cam.wrSensorReg8_8(0xDA, 0x10); // JPEG
  cam.wrSensorReg8_8(0xD7, 0x03);
  cam.wrSensorReg8_8(0xDF, 0x00);
  cam.wrSensorReg8_8(0x33, 0xA0);
  cam.wrSensorReg8_8(0x3C, 0x00);
  cam.wrSensorReg8_8(0xFF, 0x01);
  cam.wrSensorReg8_8(0x15, 0x00); // continuous PCLK
  cam.wrSensorReg8_8(0xFF, 0x00);
}

bool cam_init_once() {
  cam.set_format(JPEG);
  cam.InitCAM();
  cam.set_format(JPEG);
  cam.OV2640_set_JPEG_size(OV2640_320x240); // start QVGA for reliability
  delay(150);
  ov2640_force_jpeg();
  delay(100);

  cam.write_reg(0x03, 0x00); // ARDUCHIP_TIM VSYNC active-high (try 0x02 if needed)

  uint8_t pid=0, ver=0;
  cam.rdSensorReg8_8(0x0A, &pid);
  cam.rdSensorReg8_8(0x0B, &ver);
  // NOTE: printing to Serial also mixes with JPEG later; keep init prints minimal
  Serial.print("OV2640 PID=0x"); Serial.print(pid, HEX);
  Serial.print(" VER=0x");        Serial.println(ver, HEX);
  return pid != 0;
}

// ------------------- Ultrasonic (HC-SR04) -------------------
const int trigPin = 7;
const int echoPin = 8;

static inline long microsecondsToCentimeters(long microseconds) {
  // Speed of sound ~29 µs/cm; divide by 2 (out and back)
  return microseconds / 29 / 2;
}

long readDistanceCm(uint32_t timeout_us = 30000UL) {
  // Trigger pulse
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  // Echo with timeout to avoid long blocking
  unsigned long dur = pulseIn(echoPin, HIGH, timeout_us);
  if (dur == 0) return -1; // timeout / no return
  return microsecondsToCentimeters(dur);
}

// ------------------- Setup -------------------
void setup() {
  // Serial0: JPEG stream (binary) — use a high baud
  Serial.begin(256000);
  // Serial1: distance telemetry (text), clean channel
  Serial1.begin(115200);

  enableXCLK();

  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);
  SPI.begin();

  cam.write_reg(ARDUCHIP_TEST1, 0x55);
  Serial.print("SPI test: 0x"); Serial.println(cam.read_reg(ARDUCHIP_TEST1), HEX);

  Wire.begin();
  Wire.setClock(100000);

  // Ultrasonic pins
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  // Retry sensor ID up to 3 times
  for (int i = 0; i < 3; i++) {
    if (cam_init_once()) break;
    Serial.println("Sensor ID 0x00—reinit...");
    delay(200);
  }
}

// ------------------- Loop -------------------
void loop() {
  // 1) Read distance (quick)
  long cm = readDistanceCm();
  if (cm >= 0) {
    // Send clean text to Serial1 to avoid corrupting the JPEG stream
    Serial1.print("distance_cm: ");
    Serial1.println(cm);
  } else {
    Serial1.println("distance_cm: -1");
  }

  // 2) Capture a JPEG frame and stream to Serial (binary)
  cam.flush_fifo();
  cam.clear_fifo_flag();
  cam.write_reg(0x01, 0x00);     // single frame
  cam.start_capture();
  delay(20);

  if (!waitCaptureDone(2000)) {
    // This goes to Serial1 so it doesn't break the JPEG stream
    Serial1.println("capture_timeout");
    cam.clear_fifo_flag();
    delay(100);
    return;
  }

  uint32_t len = cam.read_fifo_length();
  if (len < 64 || len > 0x5FFFF) {
    Serial1.print("bad_fifo_len: ");
    Serial1.println(len);
    cam.flush_fifo(); cam.clear_fifo_flag();
    delay(50);
    return;
  }

  // OPTIONAL: Send a tiny header on Serial1 so your host can know a frame is starting
  // (Not on Serial to avoid corrupting JPEG)
  Serial1.print("frame_len: "); Serial1.println(len);

  uint8_t last=0, cur=0; bool in_jpeg=false; uint32_t out=0;

  digitalWrite(CS_PIN, LOW);
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0)); // try 8 MHz; drop to 4 MHz if unstable
  cam.set_fifo_burst();

  for (uint32_t i=0; i<len; i++) {
    last = cur;
    cur = SPI.transfer(0x00);
    if (!in_jpeg) {
      if (last==0xFF && cur==0xD8) {
        in_jpeg = true;
        Serial.write(0xFF); Serial.write(0xD8);
        out += 2;
      }
    } else {
      Serial.write(cur);
      out++;
      if (last==0xFF && cur==0xD9) break;
    }
  }
  SPI.endTransaction();
  digitalWrite(CS_PIN, HIGH);
  cam.clear_fifo_flag();

  // Post-frame status to Serial1 (not Serial)
  Serial1.print("jpeg_ok: "); Serial1.println(in_jpeg ? "1" : "0");
  Serial1.print("bytes_out: "); Serial1.println(out);
  Serial1.println("---");

  delay(150); // pacing between frames
}
