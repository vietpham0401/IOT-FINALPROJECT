/**
 * ============================================================================
 * DỰ ÁN CUỐI KỲ MÔN INTRODUCTION TO IOT
 * Đề tài: IoT-Based Smart Vehicle Parking System using PN532 & Phone Camera ALPR
 * Vi điều khiển: ESP32-WROOM-38P (Bản 38 chân) + Đế Mở Rộng 38P
 * ============================================================================
 *
 * CẤU HÌNH PHẦN CỨNG CHỐT (BẢN 2 SERVO CỔNG VÀO & CỔNG RA ĐỘC LẬP):
 * - 1x ESP32-WROOM-38P (38 Pins) + Đế Mở Rộng 38P
 * - 1x Module đọc thẻ NFC/RFID PN532 (Chế độ I2C: SDA=21, SCL=22, IRQ=16, RST=17)
 * - 1x Màn hình LCD 1602 hàn sẵn I2C (Dùng chung SDA=21, SCL=22)
 * - 4x Cảm biến hồng ngoại LM393:
 *      + LM393 #1: IR Ô đỗ 1 (Slot 1)    -> GPIO 32
 *      + LM393 #2: IR Ô đỗ 2 (Slot 2)    -> GPIO 33
 *      + LM393 #3: IR Cổng Vào (Gate In) -> GPIO 25
 *      + LM393 #4: IR Cổng Ra (Gate Out) -> GPIO 26
 * - 2x Động cơ Servo SG90:
 *      + Servo #1: Thanh chắn Barie CỔNG VÀO -> GPIO 13
 *      + Servo #2: Thanh chắn Barie CỔNG RA  -> GPIO 14
 * - 1x Còi Buzzer -> GPIO 27
 * - 1x Camera Điện thoại (Quét & đối chiếu biển số xe trên Web Dashboard)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

// ===================== 1. CẤU HÌNH WI-FI & FIREBASE =========================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const String FIREBASE_HOST = "https://YOUR_PROJECT_ID.firebasedatabase.app";
const String FIREBASE_AUTH = "";

// Đặt = true để bắt buộc đối chiếu biển số qua Camera ĐT trước khi mở cổng
bool REQUIRE_CAMERA_PLATE_MATCH = true;

// ===================== 2. ĐỊNH NGHĨA CHÂN TRÊN ESP32-WROOM-38P ==============
// Bus I2C dùng chung cho PN532 (0x24) và LCD 1602 (0x27)
#define I2C_SDA_PIN        21
#define I2C_SCL_PIN        22

// Chân điều khiển PN532
#define PN532_IRQ          16
#define PN532_RESET        17

// Màn hình LCD 1602 I2C
#define LCD_ADDRESS        0x27
#define LCD_COLS           16
#define LCD_ROWS           2

// 2 Động cơ Servo SG90 điều khiển 2 thanh chắn Barie Vào & Ra riêng biệt
#define SERVO_IN_PIN       13  // Servo #1: Barie CỔNG VÀO
#define SERVO_OUT_PIN      14  // Servo #2: Barie CỔNG RA
#define GATE_CLOSED_DEG    0
#define GATE_OPEN_DEG      90

// 4 Cảm biến hồng ngoại LM393 (LOW = Có xe che, HIGH = Trống)
#define NUM_SLOTS          2
const int IR_SLOT_PINS[NUM_SLOTS] = {32, 33}; // 2 cảm biến cho 2 ô đỗ xe
#define IR_GATE_IN_PIN     25                 // 1 cảm biến cho làn Cổng Vào
#define IR_GATE_OUT_PIN    26                 // 1 cảm biến cho làn Cổng Ra

// Còi báo Buzzer
#define BUZZER_PIN         27

// ===================== 3. DANH SÁCH THẺ NFC/RFID PN532 HỢP LỆ ===============
struct AuthorizedCard {
  String uid;
  String ownerName;
  String licensePlate;
  bool isParked;
};

AuthorizedCard validCards[] = {
  {"01 02 03 04", "Nguyen Van A", "29A-123.45", false},
  {"11 22 33 44", "Tran Thi B",   "30F-678.90", false},
  {"55 66 77 88", "Le Hoang C",   "51G-888.88", false}
};
const int NUM_VALID_CARDS = sizeof(validCards) / sizeof(validCards[0]);

// ===================== 4. KHỞI TẠO ĐỐI TƯỢNG & BIẾN TOÀN CỤC ================
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
Servo gateInServo;   // Servo #1 (Cổng Vào - GPIO 13)
Servo gateOutServo;  // Servo #2 (Cổng Ra  - GPIO 14)

bool slotOccupied[NUM_SLOTS] = {false, false};
bool lastSlotState[NUM_SLOTS] = {false, false};
int availableSlots = NUM_SLOTS;

bool gateInSensorActive = false;
bool gateOutSensorActive = false;

bool isGateInOpen = false;
bool isGateOutOpen = false;
unsigned long gateInOpenedTime = 0;
unsigned long gateOutOpenedTime = 0;
const unsigned long GATE_TIMEOUT_MS = 5000;

bool waitingForCameraVerify = false;
int pendingCardIndex = -1;
String pendingDirection = "CHECK_IN";
unsigned long verifyRequestTime = 0;
const unsigned long CAMERA_VERIFY_TIMEOUT_MS = 15000;

unsigned long lastCloudPoll = 0;
const unsigned long CLOUD_POLL_INTERVAL_MS = 1200;

// ===================== 5. HÀM PHỤ TRỢ ÂM THANH & HIỂN THỊ ===================
void beep(int times, int durationMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < times - 1) delay(100);
  }
}

void updateLcdDefaultScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("PARKING FREE:");
  lcd.print(availableSlots);
  lcd.print("/");
  lcd.print(NUM_SLOTS);

  lcd.setCursor(0, 1);
  lcd.print("S1:");
  lcd.print(slotOccupied[0] ? "FULL " : "FREE ");
  lcd.print("S2:");
  lcd.print(slotOccupied[1] ? "FULL" : "FREE");
}

void showLcdAlert(const String& line1, const String& line2, int beepCount, int beepMs) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  if (beepCount > 0) beep(beepCount, beepMs);
}

// ===================== 6. ĐIỀU KHIỂN 2 THANH CHẮN BARIE VÀO & RA ============
void openGateIn() {
  gateInServo.write(GATE_OPEN_DEG);
  isGateInOpen = true;
  gateInOpenedTime = millis();
  Serial.println("[GATE IN - GPIO 13] Mo Barie CONG VAO (90 deg)");
}

void closeGateIn() {
  gateInServo.write(GATE_CLOSED_DEG);
  isGateInOpen = false;
  Serial.println("[GATE IN - GPIO 13] Dong Barie CONG VAO (0 deg)");
  if (!isGateOutOpen) updateLcdDefaultScreen();
}

void openGateOut() {
  gateOutServo.write(GATE_OPEN_DEG);
  isGateOutOpen = true;
  gateOutOpenedTime = millis();
  Serial.println("[GATE OUT - GPIO 14] Mo Barie CONG RA (90 deg)");
}

void closeGateOut() {
  gateOutServo.write(GATE_CLOSED_DEG);
  isGateOutOpen = false;
  Serial.println("[GATE OUT - GPIO 14] Dong Barie CONG RA (0 deg)");
  if (!isGateInOpen) updateLcdDefaultScreen();
}

// ===================== 7. ĐỒNG BỘ FIREBASE & NHẬN KẾT QUẢ CAMERA ============
void syncSlotsToFirebase() {
  if (WiFi.status() != WL_CONNECTED || FIREBASE_HOST.indexOf("YOUR_PROJECT_ID") != -1) {
    return;
  }

  HTTPClient http;
  String url = FIREBASE_HOST + "/parking/status.json";
  if (FIREBASE_AUTH.length() > 0) url += "?auth=" + FIREBASE_AUTH;

  String payload = "{";
  payload += "\"totalSlots\":" + String(NUM_SLOTS) + ",";
  payload += "\"availableSlots\":" + String(availableSlots) + ",";
  payload += "\"gateOpen\":" + String((isGateInOpen || isGateOutOpen) ? "true" : "false") + ",";
  payload += "\"gateInOpen\":" + String(isGateInOpen ? "true" : "false") + ",";
  payload += "\"gateOutOpen\":" + String(isGateOutOpen ? "true" : "false") + ",";
  payload += "\"gateInIR\":" + String(gateInSensorActive ? "true" : "false") + ",";
  payload += "\"gateOutIR\":" + String(gateOutSensorActive ? "true" : "false") + ",";
  payload += "\"slots\":{";
  for (int i = 0; i < NUM_SLOTS; i++) {
    payload += "\"slot" + String(i + 1) + "\":" + String(slotOccupied[i] ? "true" : "false");
    if (i < NUM_SLOTS - 1) payload += ",";
  }
  payload += "}}";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.PUT(payload);
  http.end();
}

void requestCameraVerificationOnFirebase(const AuthorizedCard& card, const String& mode) {
  if (WiFi.status() != WL_CONNECTED || FIREBASE_HOST.indexOf("YOUR_PROJECT_ID") != -1) {
    return;
  }

  HTTPClient http;
  String url = FIREBASE_HOST + "/parking/cameraCheck.json";
  if (FIREBASE_AUTH.length() > 0) url += "?auth=" + FIREBASE_AUTH;

  String payload = "{";
  payload += "\"uid\":\"" + card.uid + "\",";
  payload += "\"owner\":\"" + card.ownerName + "\",";
  payload += "\"expectedPlate\":\"" + card.licensePlate + "\",";
  payload += "\"detectedPlate\":\"\",";
  payload += "\"mode\":\"" + mode + "\",";
  payload += "\"status\":\"PENDING\"";
  payload += "}";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.PUT(payload);
  http.end();
}

void pollCameraVerificationResult() {
  if (!waitingForCameraVerify || pendingCardIndex < 0) return;

  if (millis() - verifyRequestTime > CAMERA_VERIFY_TIMEOUT_MS) {
    Serial.println("[CAMERA] Het thoi gian cho quet bien so (Timeout)!");
    showLcdAlert("CAMERA TIMEOUT!", "Swipe card again", 2, 200);
    waitingForCameraVerify = false;
    pendingCardIndex = -1;
    delay(1500);
    updateLcdDefaultScreen();
    return;
  }

  if (WiFi.status() != WL_CONNECTED || FIREBASE_HOST.indexOf("YOUR_PROJECT_ID") != -1) {
    return;
  }

  HTTPClient http;
  String url = FIREBASE_HOST + "/parking/cameraCheck/status.json";
  if (FIREBASE_AUTH.length() > 0) url += "?auth=" + FIREBASE_AUTH;

  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String res = http.getString();
    res.replace("\"", "");
    res.trim();

    if (res == "MATCHED") {
      AuthorizedCard& card = validCards[pendingCardIndex];
      bool isCheckingOut = (pendingDirection == "CHECK_OUT");
      card.isParked = !isCheckingOut;

      Serial.println("[CAMERA ALPR] -> BIEN SO TRUNG KHOP: " + card.licensePlate);
      if (isCheckingOut) {
        showLcdAlert("OPEN EXIT GATE!", card.licensePlate, 1, 150);
        openGateOut(); // Mở Servo #2 (Cổng Ra - GPIO 14)
      } else {
        showLcdAlert("OPEN ENTRY GATE!", card.licensePlate, 1, 150);
        openGateIn();  // Mở Servo #1 (Cổng Vào - GPIO 13)
      }
      syncSlotsToFirebase();

      waitingForCameraVerify = false;
      pendingCardIndex = -1;
    } else if (res == "MISMATCH") {
      Serial.println("[CAMERA ALPR] -> CANH BAO: BIEN SO KHONG TRUNG KHOP VOI THE!");
      showLcdAlert("PLATE MISMATCH!", "Access Denied!", 4, 200);
      waitingForCameraVerify = false;
      pendingCardIndex = -1;
      delay(2000);
      updateLcdDefaultScreen();
    }
  }
  http.end();
}

// ===================== 8. ĐỌC 4 CẢM BIẾN HỒNG NGOẠI LM393 ===================
void readAllIrSensors() {
  bool stateChanged = false;
  int freeCount = 0;

  for (int i = 0; i < NUM_SLOTS; i++) {
    bool occupied = (digitalRead(IR_SLOT_PINS[i]) == LOW);
    slotOccupied[i] = occupied;
    if (!occupied) freeCount++;

    if (occupied != lastSlotState[i]) {
      stateChanged = true;
      lastSlotState[i] = occupied;
      Serial.printf("[IR SLOT] O do so %d: %s\n", i + 1, occupied ? "CO XE (FULL)" : "TRONG (FREE)");
    }
  }

  bool inActive = (digitalRead(IR_GATE_IN_PIN) == LOW);
  bool outActive = (digitalRead(IR_GATE_OUT_PIN) == LOW);
  if (inActive != gateInSensorActive || outActive != gateOutSensorActive) {
    gateInSensorActive = inActive;
    gateOutSensorActive = outActive;
    stateChanged = true;
  }

  availableSlots = freeCount;
  if (stateChanged) {
    if (!isGateInOpen && !isGateOutOpen && !waitingForCameraVerify) {
      updateLcdDefaultScreen();
    }
    syncSlotsToFirebase();
  }
}

// ===================== 9. ĐỌC & XỬ LÝ THẺ TỪ NFC/RFID PN532 =================
String formatUidBytes(uint8_t* uid, uint8_t uidLength) {
  String uidStr = "";
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] < 0x10) uidStr += "0";
    uidStr += String(uid[i], HEX);
    if (i < uidLength - 1) uidStr += " ";
  }
  uidStr.toUpperCase();
  return uidStr;
}

void handlePn532Scan() {
  if (waitingForCameraVerify) return;

  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;

  bool success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 40);
  if (!success) return;

  String scannedUid = formatUidBytes(uid, uidLength);
  Serial.println("=========================================");
  Serial.println("[PN532] Phat hien the NFC/RFID UID: " + scannedUid);

  int matchedIndex = -1;
  for (int i = 0; i < NUM_VALID_CARDS; i++) {
    if (validCards[i].uid.equalsIgnoreCase(scannedUid)) {
      matchedIndex = i;
      break;
    }
  }

  if (matchedIndex == -1) {
    Serial.println("[PN532] -> TU CHOI: The khong hop le!");
    showLcdAlert("ACCESS DENIED!", "UID:" + scannedUid, 3, 150);
    delay(1500);
    updateLcdDefaultScreen();
    return;
  }

  AuthorizedCard& card = validCards[matchedIndex];

  // Xác định xe đang yêu cầu mở Cổng Vào (Servo #1) hay Cổng Ra (Servo #2):
  bool wantsToExit = card.isParked;
  if (gateOutSensorActive && !gateInSensorActive) {
    wantsToExit = true;
  } else if (gateInSensorActive && !gateOutSensorActive) {
    wantsToExit = false;
  }

  if (!wantsToExit && availableSlots <= 0) {
    Serial.println("[PN532] -> TU CHOI: Bai do xe da day (2/2)!");
    showLcdAlert("PARKING FULL!", "0/2 Free Slots", 2, 250);
    delay(1500);
    updateLcdDefaultScreen();
    return;
  }

  pendingDirection = wantsToExit ? "CHECK_OUT" : "CHECK_IN";

  if (REQUIRE_CAMERA_PLATE_MATCH && WiFi.status() == WL_CONNECTED) {
    pendingCardIndex = matchedIndex;
    waitingForCameraVerify = true;
    verifyRequestTime = millis();

    Serial.println("[PN532] The hop le (" + card.licensePlate + "). Cho Camera ĐT xac thuc bien so...");
    showLcdAlert("CHECKING PLATE..", card.licensePlate, 1, 80);
    requestCameraVerificationOnFirebase(card, pendingDirection);
  } else {
    card.isParked = !wantsToExit;
    if (wantsToExit) {
      showLcdAlert("GOODBYE! (EXIT)", card.licensePlate, 1, 120);
      openGateOut(); // Mở Servo #2 (Barie Cổng Ra - GPIO 14)
    } else {
      showLcdAlert("WELCOME! (ENTRY)", card.licensePlate, 1, 120);
      openGateIn();  // Mở Servo #1 (Barie Cổng Vào - GPIO 13)
    }
    syncSlotsToFirebase();
  }
}

// ===================== 10. SETUP & LOOP CHÍNH ===============================
void setup() {
  Serial.begin(115200);
  Serial.println("\n--- KHOI DONG SMART PARKING (2 SERVO BARIE VAO/RA) ---");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  for (int i = 0; i < NUM_SLOTS; i++) {
    pinMode(IR_SLOT_PINS[i], INPUT_PULLUP);
  }
  pinMode(IR_GATE_IN_PIN, INPUT_PULLUP);
  pinMode(IR_GATE_OUT_PIN, INPUT_PULLUP);

  // Khởi tạo 2 động cơ Servo SG90 (GPIO 13 & GPIO 14)
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  gateInServo.setPeriodHertz(50);
  gateOutServo.setPeriodHertz(50);
  gateInServo.attach(SERVO_IN_PIN, 500, 2400);
  gateOutServo.attach(SERVO_OUT_PIN, 500, 2400);
  gateInServo.write(GATE_CLOSED_DEG);
  gateOutServo.write(GATE_CLOSED_DEG);

  // Khởi tạo Bus I2C (SDA=21, SCL=22) dùng chung cho LCD 1602 và PN532
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("SMART PARKING");
  lcd.setCursor(0, 1);
  lcd.print("2-GATE BARRIERS");

  // Khởi tạo module PN532
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("[PN532] CANH BAO: Khong tim thay PN532! Kiem tra cong tac I2C (SEL0=ON, SEL1=OFF).");
  } else {
    Serial.print("[PN532] Tim thay chip PN5");
    Serial.println((versiondata >> 24) & 0xFF, HEX);
    nfc.SAMConfig();
  }

  // Kết nối Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] Dang ket noi Wi-Fi");
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 8000) {
    delay(400);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Da ket noi! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] Chay o che do Offline.");
  }

  beep(2, 80);
  readAllIrSensors();
  updateLcdDefaultScreen();
}

void loop() {
  // 1. Đọc 4 cảm biến hồng ngoại LM393
  readAllIrSensors();

  // 2. Đọc thẻ NFC/RFID từ module PN532
  handlePn532Scan();

  // 3. Kiểm tra kết quả đối chiếu biển số từ Camera điện thoại
  if (waitingForCameraVerify && millis() - lastCloudPoll >= CLOUD_POLL_INTERVAL_MS) {
    lastCloudPoll = millis();
    pollCameraVerificationResult();
  }

  // 4A. Tự động đóng Barie CỔNG VÀO (Servo #1 - GPIO 13) sau khi xe đi qua khỏi IR Cổng Vào (GPIO 25)
  if (isGateInOpen) {
    if (gateInSensorActive) {
      gateInOpenedTime = millis();
    } else if (millis() - gateInOpenedTime >= GATE_TIMEOUT_MS) {
      closeGateIn();
      syncSlotsToFirebase();
    }
  }

  // 4B. Tự động đóng Barie CỔNG RA (Servo #2 - GPIO 14) sau khi xe đi qua khỏi IR Cổng Ra (GPIO 26)
  if (isGateOutOpen) {
    if (gateOutSensorActive) {
      gateOutOpenedTime = millis();
    } else if (millis() - gateOutOpenedTime >= GATE_TIMEOUT_MS) {
      closeGateOut();
      syncSlotsToFirebase();
    }
  }
}
