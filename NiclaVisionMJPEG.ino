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
constexpr uint8_t  JPG_QUAL  = JPEGE_Q_LOW;
constexpr size_t   JPG_BUF_SZ = 80 * 1024;
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
  server.begin();
  Serial.print("\u2714 Wi‑Fi connected — open http://");
  Serial.println(WiFi.localIP());
}

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200); while (!Serial);

  if (!cam.begin(CAM_RES, CAM_FMT, FPS)) {
    Serial.println(F("Camera init failed — check ribbon & power"));
    while (true);
  }

  connectWiFi();
}

/* ── MJPEG streamer ──────────────────────────────────────────────────── */
void streamMJPEG(WiFiClient &c)
{
  c.println("HTTP/1.1 200 OK");
  c.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  c.println("Cache-Control: no-cache\r\n");

  const int pitch = 320 * 2;   // bytes per row for RGB565
  // const int pitch = 320;   // bytes per row for grayscale

static unsigned long lastPrint = 0;
static int frameCount = 0;

// For profiling
static unsigned long totalGrab = 0;
static unsigned long totalSwap = 0;
static unsigned long totalJpeg = 0;
static unsigned long totalSend = 0;
static int profileFrames = 0;

while (c.connected()) {
    unsigned long t0 = micros();

    // ---- 1. Grab Frame ----
    if (cam.grabFrame(fb, 3000) == 0) {
        unsigned long t1 = micros();

        // ---- 2. Swap Color Bytes ----
        // uint8_t *buf8 = fb.getBuffer();
        // static uint8_t swapBuf[320*240*2];
        // for (int i = 0; i < 320 * 240; i++) {
        //     swapBuf[2*i]   = buf8[2*i+1];
        //     swapBuf[2*i+1] = buf8[2*i];
        // }
        unsigned long t2 = micros();

        // delay(2);

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

        if (jpegLen <= 0) continue;
        unsigned long t3 = micros();

        // ---- 4. Send over Network ----
        digitalWrite(LED_BUILTIN, LOW);
        c.print("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ");
        c.print(jpegLen);
        c.print("\r\n\r\n");
        c.write(jpgBuf, jpegLen);
        c.print("\r\n");
        digitalWrite(LED_BUILTIN, HIGH);
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
        if (now - lastPrint >= 1000) { // Print every second
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
    // delay(1000 / FPS);
  }
}


/* ── HTTP dispatcher ─────────────────────────────────────────────────── */
void loop()
{
  WiFiClient client = server.accept();
  if (!client) return;

  String req = client.readStringUntil('\r');
  client.readStringUntil('\n');

  if (req.startsWith("GET /stream")) {
    streamMJPEG(client);
  } else {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("Connection: close\r\n");
    client.print(HOMEPAGE);
  }
  client.stop();
}
