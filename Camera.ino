#include <Wire.h>
#include <SPI.h>
#include <ArduCAM.h>
#include "memorysaver.h"

#define OV2640_MINI_2MP
#define CS_PIN 53
ArduCAM cam(OV2640, CS_PIN);

// 8 MHz XCLK on Mega pin 11
void enableXCLK() {
  pinMode(11, OUTPUT);
  TCCR1A = _BV(COM1A0);
  TCCR1B = _BV(WGM12) | _BV(CS10);
  OCR1A  = 1; // ~8 MHz (try 1 for ~4 MHz if unstable)
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
  cam.OV2640_set_JPEG_size(OV2640_320x240); // start QVGA
  delay(150);
  ov2640_force_jpeg();
  delay(100);

  cam.write_reg(0x03, 0x00);               // ARDUCHIP_TIM VSYNC active-high (try 0x02 if needed)

  uint8_t pid=0, ver=0;
  cam.rdSensorReg8_8(0x0A, &pid);
  cam.rdSensorReg8_8(0x0B, &ver);
  Serial.print("OV2640 PID=0x"); Serial.print(pid, HEX);
  Serial.print(" VER=0x");        Serial.println(ver, HEX);
  return pid != 0;
}

void setup() {
  Serial.begin(256000);
  enableXCLK();

  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);
  SPI.begin();

  cam.write_reg(ARDUCHIP_TEST1, 0x55);
  Serial.print("SPI test: 0x"); Serial.println(cam.read_reg(ARDUCHIP_TEST1), HEX);

  Wire.begin();
  Wire.setClock(100000);

  // Retry init up to 3x if ID returns 0
  for (int i = 0; i < 3; i++) {
    if (cam_init_once()) break;
    Serial.println("Sensor ID 0x00—reinit...");
    delay(200);
  }
}

void loop() {
  cam.flush_fifo();
  cam.clear_fifo_flag();
  cam.write_reg(0x01, 0x00);                // single frame
  cam.start_capture();
  delay(20);

  if (!waitCaptureDone(2000)) {
    Serial.println("Capture TIMEOUT");
    cam.clear_fifo_flag();
    return;
  }

  uint32_t len = cam.read_fifo_length();
  if (len < 256 || len > 0x5FFFF) { // sanity
  Serial.print("Odd FIFO len: "); Serial.println(len);
  cam.flush_fifo(); cam.clear_fifo_flag();
  delay(50);
  return;
}
  Serial.print("FIFO length = "); Serial.println(len);
  if (len < 64) { Serial.println("Too small; check power/RESET/PWDN."); delay(500); return; }

  uint8_t last=0, cur=0; bool in_jpeg=false; uint32_t out=0;

  digitalWrite(CS_PIN, LOW);
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0)); // safe first
  cam.set_fifo_burst();

  for (uint32_t i=0; i<len; i++) {
    last = cur;
    cur = SPI.transfer(0x00);
    if (!in_jpeg) {
      if (last==0xFF && cur==0xD8) { in_jpeg=true; Serial.write(0xFF); Serial.write(0xD8); out+=2; }
    } else {
      Serial.write(cur); out++;
      if (last==0xFF && cur==0xD9) break;
    }
  }
  SPI.endTransaction();
  digitalWrite(CS_PIN, HIGH);
  cam.clear_fifo_flag();

  Serial.print("\r\nSOI found? "); Serial.println(in_jpeg ? "YES" : "NO");
  Serial.print("Bytes output: "); Serial.println(out);
  Serial.println("----");

  delay(800); // single-shot pacing
}

