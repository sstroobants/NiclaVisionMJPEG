/*  NiclaVision_MJPEGStream.ino — rev 6 (2025‑07‑16)
 *  ───────────────────────────────────────────────────────────────
 *  Fixes
 *  1.  Correct colour order without per‑pixel swapping:
 *      • Define JPEGE_PIXEL_RGB565 locally if the installed
 *        JPEGENC copy is too old to ship it, then use it.
 *  2.  Deal with stray grey horizontal bars (GC2145 blank lines):
 *      • Discard a frame if its JPEG size differs >20 % from the
 *        running average; this drops corrupted captures.
 *      • Optionally refire the sensor once after a bad frame.
 */

#include "camera.h"
#ifdef ARDUINO_NICLA_VISION
  #include "gc2145.h"
  GC2145    galaxyCore;
  Camera    cam(galaxyCore);
#else
  #error "This sketch targets Arduino Nicla Vision only"
#endif

#include <WiFi.h>
#include <WiFiUdp.h>
WiFiUDP udp;

// ---- Set your PC/server's IP and port here ----
IPAddress destIP(10,42,0,1); // Replace with your PC's IP!
const uint16_t destPort = 5005;
#include "arduino_secrets.h"

/* ── JPEG encoder ─────────────────────────────────────────────────────── */
#ifdef JPEG
  #undef JPEG
#endif
#include <JPEGENC.h>



static JPEGENC     jpg;
static JPEGENCODE  ctx;

/* ── Parameters ─────────────────────────────────────────────────────── */
constexpr uint8_t  CAM_RES   = CAMERA_R320x240;
constexpr uint8_t  CAM_FMT   = CAMERA_RGB565;
// constexpr uint8_t  CAM_FMT   = CAMERA_GRAYSCALE;
constexpr uint8_t  FPS       = 30;
constexpr uint8_t  JPG_QUAL  = JPEGE_Q_MED;
constexpr size_t   JPG_BUF_SZ = 40 * 1024;
static uint8_t     jpgBuf[JPG_BUF_SZ];

WiFiServer server(80);
FrameBuffer fb;

/* ── HTML viewer ─────────────────────────────────────────────────────── */
const char HOMEPAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset=utf-8>
<title>Nicla Vision MJPEG</title><style>
html,body{margin:0;height:100%;display:flex;align-items:center;justify-content:center;background:#000}
img{image-rendering:pixelated;outline:2px solid #222}
</style></head><body>
<img src="stream" width="320" height="240" alt="Nicla stream">
</body></html>)HTML";

/* ── Wi‑Fi helper ─────────────────────────────────────────────────────── */
void connectWiFi()
{
  while (WiFi.begin(SECRET_SSID, SECRET_PASS) != WL_CONNECTED) {
    delay(500);
  }

  udp.begin(5005); 

  server.begin();
  Serial.print("\u2714 Wi‑Fi connected — open http://");
  Serial.println(WiFi.localIP());
}

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  delay(100);

  if (!cam.begin(CAM_RES, CAM_FMT, FPS)) {
    Serial.println(F("Camera init failed — check ribbon & power"));
    while (true);
  }
  cam.setVerticalFlip(true);

  connectWiFi();
}

void loop()
{
    static unsigned long lastPrint = 0;
    static int frameCount = 0;
    static unsigned long totalGrab = 0;
    static unsigned long totalSwap = 0;
    static unsigned long totalJpeg = 0;
    static unsigned long totalSend = 0;
    static int profileFrames = 0;

    const int pitch = 320 * 2;   // bytes per row for RGB565

    while (WiFi.status() == WL_CONNECTED) {
        unsigned long t0 = micros();

        // ---- 1. Grab Frame ----
        if (cam.grabFrame(fb, 3000) == 0) {
            unsigned long cam_capture_ms = millis();
            unsigned long t1 = micros();

            // ---- 2. Swap Color Bytes ----
            // Why is this necessary?
            uint8_t *buf8 = fb.getBuffer();
            static uint8_t swapBuf[320*240*2];
            for (int i = 0; i < 320 * 240; i++) {
                swapBuf[2*i]   = buf8[2*i+1];
                swapBuf[2*i+1] = buf8[2*i];
            }
            unsigned long t2 = micros();

            // ---- 3. JPEG Encode ----
            if (jpg.open(jpgBuf, JPG_BUF_SZ) != JPEGE_SUCCESS) {
                Serial.println(F("JPEG buffer too small – raise JPG_BUF_SZ"));
                while (true);
            }
            if (jpg.encodeBegin(&ctx, 320, 240,
                                JPEGE_PIXEL_RGB565,
                                JPEGE_SUBSAMPLE_420,
                                JPG_QUAL) != JPEGE_SUCCESS) {
                continue; // shouldn’t happen
            }
            jpg.addFrame(&ctx, swapBuf, pitch);
            int jpegLen = jpg.close();

            // Serial.print("Sending JPEG of size: ");
            // Serial.println(jpegLen);

            if (jpegLen < 1000 || jpegLen > 13000) {
                Serial.print("Discarding frame with odd JPEG size: ");
                Serial.println(jpegLen);
                continue; // Skip this frame!
            }
            unsigned long t3 = micros();

            // ---- 4. Send via UDP ----
            // We'll send 4 bytes of timestamp (ms) + JPEG data

            const int MAX_UDP_PAYLOAD = 500; // Safe for most networks

            // After encoding JPEG...
            int total_chunks = (jpegLen + MAX_UDP_PAYLOAD - 1) / MAX_UDP_PAYLOAD;
            for (int i = 0; i < total_chunks; i++) {
                udp.beginPacket(destIP, destPort);
                udp.write((const uint8_t*)&cam_capture_ms, 4);  // timestamp
                uint16_t chunk_num = i;
                udp.write((const uint8_t*)&chunk_num, 2);
                uint16_t chunk_total = total_chunks;
                udp.write((const uint8_t*)&chunk_total, 2);
                int start = i * MAX_UDP_PAYLOAD;
                int this_len = min(MAX_UDP_PAYLOAD, jpegLen - start);
                udp.write(jpgBuf + start, this_len);
                udp.endPacket();
            }
            // udp.beginPacket(destIP, destPort);
            // udp.write((const uint8_t*)&cam_capture_ms, sizeof(cam_capture_ms));
            // udp.write(jpgBuf, jpegLen);
            // udp.endPacket();
            unsigned long t4 = micros();

            // ---- 5. Accumulate Times ----
            totalGrab += (t1 - t0);
            totalSwap += (t2 - t1);
            totalJpeg += (t3 - t2);
            totalSend += (t4 - t3);
            profileFrames++;

            // --- Framerate debug ---
            frameCount++;
            unsigned long now = millis();
            if (now - lastPrint >= 1000) {
                Serial.print(F("Actual FPS: "));
                Serial.println(frameCount);
                Serial.print(F("Avg grab: "));
                Serial.print(totalGrab / profileFrames);
                Serial.print(F("us, swap: "));
                Serial.print(totalSwap / profileFrames);
                Serial.print(F("us, JPEG: "));
                Serial.print(totalJpeg / profileFrames);
                Serial.print(F("us, send: "));
                Serial.print(totalSend / profileFrames);
                Serial.println(F("us"));
                frameCount = 0;
                lastPrint = now;
                totalGrab = totalSwap = totalJpeg = totalSend = 0;
                profileFrames = 0;
            }
        }
    }
}


/* ── HTTP dispatcher ─────────────────────────────────────────────────── */
// void loop()
// {
//   WiFiClient client = server.accept();
//   if (!client) return;

//   String req = client.readStringUntil('\r');
//   client.readStringUntil('\n');

//   if (req.startsWith("GET /stream")) {
//     streamMJPEG(client);
//   } else {
//     client.println("HTTP/1.1 200 OK");
//     client.println("Content-Type: text/html");
//     client.println("Connection: close\r\n");
//     client.print(HOMEPAGE);
//   }
//   client.stop();
// }
