# IoT-Based Smart Vehicle Parking System using PN532 RFID/NFC & Phone Camera ALPR

Đồ án môn học **Introduction to IoT**: Hệ thống bãi đỗ xe thông minh xác thực 2 lớp (**2-Factor Authentication**) kết hợp giữa **Module NFC/RFID PN532**, **Nhận diện biển số xe bằng Camera điện thoại (OCR/ALPR)**, **Cảm biến hồng ngoại (IR)**, **Vi điều khiển ESP32** và **Web Dashboard thời gian thực**.

---

## 🌟 Điểm Nhấn Công Nghệ (Xác Thực 2 Lớp - 2FA Parking)

1. **Lớp 1 - Đọc thẻ từ NFC/RFID bằng module PN532 (Giao tiếp I2C):**
   - Khi xe đến cổng và quẹt thẻ lên **PN532**, ESP32 tra cứu mã `UID` của thẻ để tìm **Biển số xe đã đăng ký** gắn với thẻ đó (ví dụ thẻ `01 02 03 04` đăng ký biển số `29A-123.45`).
2. **Lớp 2 - Camera Điện Thoại Nhận Diện Biển Số Xe (OCR Tesseract.js):**
   - **Camera điện thoại** (kết nối trực tiếp trên Web Dashboard hoặc qua ứng dụng **IP Webcam**) chụp biển số gắn trước đầu xe và dùng trí tuệ nhân tạo **OCR (Optical Character Recognition)** để đọc chuỗi ký tự biển số thực tế.
3. **Đối chiếu & Quyết định Mở Cổng:**
   - **Nếu Biển số Camera == Biển số Thẻ PN532 (`MATCHED`):** Mở thanh chắn Barie (Servo quay 90°), LCD báo `PLATE MATCH`.
   - **Nếu Biển số Camera != Biển số Thẻ PN532 (`MISMATCH`):** Phát cảnh báo tráo thẻ/xe gian trên cả Web Dashboard và còi Buzzer, **giữ đóng cổng Barie**.

---

## 🛠️ 1. Danh Sách Phần Cứng Cần Thiết (Hardware BOM)

| STT | Linh kiện | Thông số kỹ thuật | Số lượng | Ghi chú |
| :-: | :--- | :--- | :-: | :--- |
| 1 | **Vi điều khiển ESP32** | ESP32-WROOM-32 DevKit V1 (30/38 chân) | 1 | Trung tâm xử lý & kết nối Wi-Fi |
| 2 | **Module NFC/RFID PN532** | PN532 NFC/RFID V3 (13.56MHz, hỗ trợ I2C/SPI/HSU) | 1 | Đọc thẻ Mifare, NFC và điện thoại có NFC |
| 3 | **Thẻ từ RFID/NFC 13.56MHz** | Thẻ trắng S50 hoặc móc khóa NFC | 3 - 5 | Cấp cho các xe hợp lệ & 1 thẻ lạ để test |
| 4 | **Điện thoại thông minh (Smartphone)** | Điện thoại Android/iOS có sẵn của bạn + Giá đỡ ĐT | 1 | Làm Camera ALPR soi biển số xe tại cổng vào |
| 5 | **Cảm biến hồng ngoại (IR)** | Module IR Obstacle Avoidance (3 chân) | 4 | 3 cảm biến cho 3 ô đỗ + 1 cảm biến an toàn tại cổng |
| 6 | **Động cơ RC Servo** | SG90 hoặc MG90S (góc quay 0° - 180°) | 1 | Đóng/mở thanh chắn Barie |
| 7 | **Màn hình LCD 1602 + I2C** | LCD 16x2 hàn sẵn module chuyển đổi I2C | 1 | Hiển thị số chỗ trống & kết quả đối chiếu biển số |
| 8 | **Còi Buzzer** | Active Buzzer 3.3V - 5V | 1 | Báo hiệu quẹt thẻ & cảnh báo sai biển số |
| 9 | **Breadboard & Dây cắm** | Breadboard 830 lỗ + Dây Jumper | 1 bộ | Đấu nối mạch không cần hàn |

---

## 🔌 2. Sơ Đồ Đấu Nối Chân (ESP32 + PN532 I2C Pinout)

> **Lưu ý cấu hình công tắc gạt (DIP Switch) trên mạch PN532 sang chế độ I2C:**
> Trên bo mạch PN532 màu đỏ có 1 cụm công tắc nhỏ 2 kênh (`SEL0` và `SEL1`), hãy gạt:
> - **Kênh 1 (`SEL0`):** **`ON` (`1`)**
> - **Kênh 2 (`SEL1`):** **`OFF` (`0`)**
>
> Khi ở chế độ I2C, **PN532 (địa chỉ `0x24`)** và **LCD 1602 (địa chỉ `0x27`)** dùng chung luôn 2 chân `SDA (GPIO 21)` và `SCL (GPIO 22)` của ESP32 cực kỳ gọn gàng!

| Thiết bị | Chân trên thiết bị | Chân trên ESP32 | Chức năng / Ghi chú |
| :--- | :---: | :---: | :--- |
| **PN532 NFC/RFID** *(Chế độ I2C)* | `SDA` | `GPIO 21` | Dùng chung bus I2C Data với LCD |
| | `SCL` | `GPIO 22` | Dùng chung bus I2C Clock với LCD |
| | `IRQ` | `GPIO 16` | Chân ngắt báo có thẻ (Interrupt) |
| | `RST (RSTO)` | `GPIO 17` | Chân Reset PN532 |
| | `VCC` | `3.3V` (hoặc `5V`) | PN532 hỗ trợ cả 3.3V và 5V |
| | `GND` | `GND` | Mass chung |
| **LCD 1602 I2C** | `SDA` | `GPIO 21` | Dùng chung bus I2C Data với PN532 |
| | `SCL` | `GPIO 22` | Dùng chung bus I2C Clock với PN532 |
| | `VCC` | `VIN (5V)` | Cấp 5V để chữ sáng rõ |
| | `GND` | `GND` | Mass chung |
| **Servo SG90 (Barie)** | `Signal (Cam)` | `GPIO 13` | Xung PWM điều khiển góc quay |
| | `VCC (Đỏ)` | `VIN (5V)` | Nguồn 5V |
| | `GND (Nâu)` | `GND` | Mass chung |
| **Cảm biến IR Ô Đỗ** | `OUT` (Ô 1, 2, 3) | `GPIO 32, 33, 25` | `LOW` = Có xe, `HIGH` = Trống |
| **Cảm biến IR Cổng** | `OUT` (Cổng) | `GPIO 26` | Giữ thanh chắn không đóng khi xe đang đi qua |
| **Còi Buzzer** | `(+)` | `GPIO 27` | Báo âm thanh |

---

## 💻 3. Danh Sách Phần Mềm Cần Thiết

1. **Arduino IDE (2.x)** kèm các thư viện (cài trong *Library Manager*):
   - **`Adafruit PN532`** *(by Adafruit)* — Thư viện chuẩn điều khiển module PN532.
   - **`ESP32Servo`** *(by Kevin Harrington)* — Điều khiển Servo Barie.
   - **`LiquidCrystal I2C`** *(by Frank de Brabander)* — Điều khiển màn hình LCD 1602.
2. **Cách kết nối Camera Điện Thoại với Web Dashboard (`web-dashboard/index.html`):**
   - **Cách A (Trực tiếp):** Mở Web Dashboard hoặc dùng phần mềm **DroidCam / Iriun Webcam** biến điện thoại thành Webcam máy tính, sau đó bấm nút **"📱 Bật Camera Điện Thoại / Webcam"** trên giao diện Web.
   - **Cách B (Không dây qua Wi-Fi):** Cài app miễn phí **IP Webcam** (trên Android) hoặc **IP Camera Lite** (trên iOS), bật *"Start Server"* rồi copy đường link `http://192.168.1.x:8080/shot.jpg` dán vào mục **"🌐 Dùng IP Webcam App"** trên Web Dashboard.
   - **Mẹo làm mô hình thuyết trình:** In 3 mảnh giấy nhỏ ghi biển số `29A-123.45`, `30F-678.90`, `51G-888.88` chữ đen nền trắng rõ nét dán lên đầu 3 chiếc xe đồ chơi để Camera điện thoại nhận diện OCR nhanh và chính xác 100%!
