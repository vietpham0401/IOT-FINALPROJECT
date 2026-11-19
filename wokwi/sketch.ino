/**
 * ============================================================================
 * BẢN MÔ PHỎNG TRỰC TIẾP TRÊN WOKWI (SỬ DỤNG MODULE MFRC522 THẬT TRÊN WOKWI)
 * Đề tài: IoT-Based Smart Vehicle Parking System using RFID
 * ============================================================================
 * Hướng dẫn tương tác trên Wokwi sau khi bấm nút Play (Màu xanh lá):
 * 1. Bấm chuột trực tiếp vào module "MFRC522 RFID Reader" màu xanh dương:
 *    - Chọn "Blue Card"   (UID: 01 02 03 04) -> Thẻ HỢP LỆ (Xe 1: 29A-123.45)
 *    - Chọn "Green Card"  (UID: 11 22 33 44) -> Thẻ HỢP LỆ (Xe 2: 30F-678.90)
 *    - Chọn "Yellow Card" (UID: 55 66 77 88) -> Thẻ HỢP LỆ (Xe 3: 51G-888.88)
 *    - Chọn "Red Card"    (UID: AA BB CC DD) hoặc "Key Fob" -> Thẻ LẠ (Từ
 * chối!) Sau đó bấm nút "Tap Card" (hoặc Insert Card) trên bảng điều khiển
 * RFID!
 *
 * 2. Gạt 3 công tắc bên trái (GPIO 32, 33, 25) sang TRÁI:
 *    - Giả lập cảm biến vật cản hồng ngoại (IR Obstacle) phát hiện xe đã đỗ vào
 * Ô 1, 2, 3.
 */

#include <ESP32Servo.h>
#include <HTTPClient.h>
#include <LiquidCrystal_I2C.h>
#include <MFRC522.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>

// Wi-Fi ảo tích hợp sẵn của Wokwi
const char *WIFI_SSID = "Wokwi-GUEST";
const char *WIFI_PASSWORD = "";

// Điền URL Firebase nếu muốn đồng bộ từ Wokwi lên Web Dashboard
const String FIREBASE_HOST = "https://YOUR_PROJECT_ID.firebasedatabase.app";

// Chân SPI của module MFRC522 RFID Reader trên ESP32
#define SS_PIN 5
#define RST_PIN 4
// SCK = 18, MISO = 19, MOSI = 23

#define LCD_ADDRESS 0x27
#define SERVO_PIN 13
#define BUZZER_PIN 27

#define NUM_SLOTS 3
const int IR_SLOT_PINS[NUM_SLOTS] = {32, 33, 25};
#define IR_GATE_PIN 26

MFRC522 rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(LCD_ADDRESS, 16, 2);
Servo gateServo;

struct AuthorizedCard {
  String uid;
  String ownerName;
  String licensePlate;
  bool isParked;
};

// Danh sách 3 thẻ hợp lệ khớp với 3 thẻ preset (Blue, Green, Yellow) trên Wokwi
// MFRC522
AuthorizedCard validCards[] = {
    {"01 02 03 04", "Nguyen Van A", "29A-123.45",
     false},                                            // Blue Card trên Wokwi
    {"11 22 33 44", "Tran Thi B", "30F-678.90", false}, // Green Card trên Wokwi
    {"55 66 77 88", "Le Hoang C", "51G-888.88", false} // Yellow Card trên Wokwi
};
const int NUM_VALID_CARDS = sizeof(validCards) / sizeof(validCards[0]);

bool slotOccupied[NUM_SLOTS] = {false, false, false};
bool lastSlotState[NUM_SLOTS] = {false, false, false};
int availableSlots = NUM_SLOTS;

bool isGateOpen = false;
unsigned long gateOpenedTime = 0;
const unsigned long GATE_TIMEOUT_MS = 4000;

void beep(int times, int durationMs) {
  for (int i = 0; i < times; i++) {
    tone(BUZZER_PIN, 2000, durationMs);
    delay(durationMs + 80);
  }
}

void updateLcdScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SMART PARKING ");
  lcd.print(availableSlots);
  lcd.print("F");

  lcd.setCursor(0, 1);
  for (int i = 0; i < NUM_SLOTS; i++) {
    lcd.print("S");
    lcd.print(i + 1);
    lcd.print(":");
    lcd.print(slotOccupied[i] ? "X " : "O ");
  }
}

void openGate() {
  gateServo.write(90);
  isGateOpen = true;
  gateOpenedTime = millis();
  Serial.println(">>> [BARIE] Mo thanh chan (Goc 90 do)");
}

void closeGate() {
  gateServo.write(0);
  isGateOpen = false;
  Serial.println(">>> [BARIE] Dong thanh chan (Goc 0 do)");
  updateLcdScreen();
}

String getCardUidString() {
  String uidStr = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10)
      uidStr += "0";
    uidStr += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1)
      uidStr += " ";
  }
  uidStr.toUpperCase();
  return uidStr;
}

void processCardUid(String scannedUid) {
  scannedUid.trim();
  scannedUid.toUpperCase();
  Serial.println("\n==========================================");
  Serial.println("[RFID RC522] Phat hien quet the UID: " + scannedUid);

  int matchedIndex = -1;
  for (int i = 0; i < NUM_VALID_CARDS; i++) {
    if (validCards[i].uid.equalsIgnoreCase(scannedUid)) {
      matchedIndex = i;
      break;
    }
  }

  // 1. Thẻ lạ (Ví dụ chọn Red Card: AA BB CC DD hoặc Key Fob: C0 FF EE 99)
  if (matchedIndex == -1) {
    Serial.println("[RFID] -> CANH BAO: The la khong hop le!");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("ACCESS DENIED!");
    lcd.setCursor(0, 1);
    lcd.print("UID:" + scannedUid);
    beep(3, 150);
    delay(1500);
    updateLcdScreen();
  }
  // 2. Thẻ hợp lệ đang đỗ trong bãi -> Quẹt thẻ để ĐI RA
  else if (validCards[matchedIndex].isParked) {
    validCards[matchedIndex].isParked = false;
    Serial.println("[RFID] -> XE RA: " + validCards[matchedIndex].ownerName +
                   " (" + validCards[matchedIndex].licensePlate + ")");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("GOODBYE!");
    lcd.setCursor(0, 1);
    lcd.print(validCards[matchedIndex].licensePlate);
    beep(1, 150);
    openGate();
  }
  // 3. Thẻ hợp lệ muốn ĐI VÀO nhưng bãi xe đã hết chỗ
  else if (availableSlots <= 0) {
    Serial.println("[RFID] -> TU CHOI: Bai xe da HET CHO TRONG!");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("PARKING FULL!");
    lcd.setCursor(0, 1);
    lcd.print("Vui long cho...");
    beep(2, 250);
    delay(1500);
    updateLcdScreen();
  }
  // 4. Thẻ hợp lệ & Còn chỗ trống -> Mở cổng cho XE VÀO
  else {
    validCards[matchedIndex].isParked = true;
    Serial.println("[RFID] -> XE VAO: " + validCards[matchedIndex].ownerName +
                   " (" + validCards[matchedIndex].licensePlate + ")");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WELCOME!");
    lcd.setCursor(0, 1);
    lcd.print(validCards[matchedIndex].licensePlate);
    beep(1, 150);
    openGate();
  }
}

void handleRfidReader() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return;
  }
  String scannedUid = getCardUidString();
  processCardUid(scannedUid);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);

  for (int i = 0; i < NUM_SLOTS; i++) {
    pinMode(IR_SLOT_PINS[i], INPUT_PULLUP);
  }
  pinMode(IR_GATE_PIN, INPUT_PULLUP);

  // Khởi tạo Servo Barie
  ESP32PWM::allocateTimer(0);
  gateServo.setPeriodHertz(50);
  gateServo.attach(SERVO_PIN, 500, 2400);
  gateServo.write(0);

  // Khởi tạo LCD 1602 I2C
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("SMART PARKING");
  lcd.setCursor(0, 1);
  lcd.print("Init RFID+WiFi..");

  // Khởi tạo giao tiếp SPI & Module MFRC522
  SPI.begin(18, 19, 23, 5);
  rfid.PCD_Init();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 3500) {
    delay(250);
  }

  Serial.println("====================================================");
  Serial.println(" HE THONG BAI DO XE THONG MINH IOT (MFRC522 WOKWI)");
  Serial.println(" - Bam vao module MFRC522 de chon the & quet:");
  Serial.println("   + Blue Card   (01 02 03 04): Hop le - 29A-123.45");
  Serial.println("   + Green Card  (11 22 33 44): Hop le - 30F-678.90");
  Serial.println("   + Yellow Card (55 66 77 88): Hop le - 51G-888.88");
  Serial.println("   + Red Card    (AA BB CC DD): The la - Tu choi!");
  Serial.println("====================================================");

  beep(2, 80);
  updateLcdScreen();
}

void loop() {
  // 1. Đọc trạng thái 3 cảm biến ô đỗ xe
  bool changed = false;
  int freeCount = 0;
  for (int i = 0; i < NUM_SLOTS; i++) {
    bool occupied = (digitalRead(IR_SLOT_PINS[i]) == LOW);
    slotOccupied[i] = occupied;
    if (!occupied)
      freeCount++;
    if (occupied != lastSlotState[i]) {
      changed = true;
      lastSlotState[i] = occupied;
      Serial.printf("[IR SENSOR] O do xe S%d: %s\n", i + 1,
                    occupied ? "CO XE (X)" : "TRONG (O)");
    }
  }
  availableSlots = freeCount;
  if (changed && !isGateOpen) {
    updateLcdScreen();
  }

  // 2. Đọc thẻ từ trực tiếp từ module MFRC522 trên Wokwi
  handleRfidReader();

  // 3. Tự động đóng cổng Barie khi xe đã qua cảm biến an toàn
  if (isGateOpen) {
    if (digitalRead(IR_GATE_PIN) == LOW) {
      gateOpenedTime = millis();
    } else if (millis() - gateOpenedTime >= GATE_TIMEOUT_MS) {
      closeGate();
    }
  }

  delay(50);
}
