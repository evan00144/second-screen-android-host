
# Product Requirements Document

## USB Second Display for Android — V0.1

**Status:** MVP
**Platform Host:** Windows 10/11
**Platform Client:** Android Tablet
**Primary Test Device:** Redmi Pad 2 Pro
**Primary Connection:** USB
**Target:** Extended Display dengan latency rendah

---

# 1. Product Overview

Aplikasi ini memungkinkan tablet Android digunakan sebagai **monitor kedua Windows melalui kabel USB**.

Windows harus mengenali tablet sebagai display tambahan yang sesungguhnya sehingga pengguna dapat menggunakan mode:

* Extend desktop
* Duplicate display
* Mengatur posisi monitor melalui Windows Display Settings
* Memindahkan window dari monitor utama ke tablet

Tablet Android menerima output display tersebut sebagai compressed video stream dan menampilkannya secara fullscreen dengan latency serendah mungkin.

Produk secara konsep mirip:

* SuperDisplay
* Duet Display
* spacedesk

Tetapi MVP difokuskan hanya pada:

> **Windows → Android via USB dengan latency rendah.**

---

# 2. Problem

Tablet Android memiliki:

* layar besar
* resolusi tinggi
* touchscreen
* hardware video decoder

Tetapi secara umum tablet seperti Redmi Pad 2 Pro tidak menerima DisplayPort input secara langsung.

Artinya:

```text
Laptop USB-C
    ↓
Android Tablet

≠ external monitor native
```

Diperlukan software layer yang:

```text
Windows
   ↓
Virtual Monitor
   ↓
Capture framebuffer
   ↓
Hardware encode
   ↓
USB transport
   ↓
Android hardware decode
   ↓
Tablet display
```

---

# 3. Product Goal

Goal utama MVP:

> Membuat Android tablet terasa seperti monitor eksternal Windows dengan koneksi USB.

Prioritas:

```text
1. Low latency
2. Stable 60 FPS
3. Extended desktop
4. Plug-and-connect sederhana
5. Image quality bagus
```

Bukan prioritas MVP:

```text
Touch
Stylus
Audio
Wi-Fi
Mac
120 Hz
Multi-tablet
Remote internet display
```

---

# 4. Target User Flow

User melakukan:

```text
1. Install Windows Host
2. Install Android Client
3. Enable USB Debugging
4. Hubungkan tablet ke PC dengan USB
5. Jalankan Android App
6. Jalankan Windows Host
7. Klik Connect
```

Kemudian Windows mendeteksi:

```text
Display 1
Main Monitor

Display 2
Android Tablet
```

User membuka:

```text
Settings
→ System
→ Display
```

dan dapat memilih:

```text
Extend these displays
```

Tablet kemudian langsung menampilkan desktop kedua.

---

# 5. Core MVP Requirements

## 5.1 Virtual Display

Windows harus mempunyai virtual display menggunakan:

```text
Windows Indirect Display Driver
IddCx
UMDF
```

Driver bertanggung jawab membuat monitor virtual.

Target awal:

```text
1920 × 1200 @ 60 Hz
```

Setelah stabil:

```text
2560 × 1600 @ 60 Hz
```

Target native Redmi Pad 2 Pro:

```text
2560 × 1600
16:10
```

Supported mode MVP:

```text
1920×1080 @ 60
1920×1200 @ 60
2560×1600 @ 60
```

Tidak perlu support banyak resolution pada prototype pertama.

---

# 6. Connection Architecture

MVP menggunakan:

```text
USB Cable
+
ADB
+
TCP Tunnel
```

Bukan Wi-Fi.

Android terhubung secara fisik ke Windows menggunakan USB.

Contoh:

```bash
adb reverse tcp:5000 tcp:5000
```

Sehingga Android dapat mengakses Windows Host melalui:

```text
127.0.0.1:5000
```

Tetapi data sebenarnya berjalan:

```text
Android
   │
 USB
   │
 Windows
```

Keuntungan approach ini:

* tidak perlu implement custom USB protocol di awal
* development jauh lebih cepat
* tidak bergantung Wi-Fi
* latency rendah
* bandwidth konsisten
* mudah debugging

ADB adalah dependency **developer MVP**, bukan desain final produk.

---

# 7. Future USB Architecture

Setelah MVP terbukti bekerja:

```text
V0.1
ADB USB Tunnel
```

kemudian:

```text
V0.2
Native USB Transport
```

Goal production:

```text
Plug tablet
↓
Open app
↓
Connect
```

tanpa:

```text
Android Developer Options
USB Debugging
ADB installation
```

Dengan demikian ADB dianggap:

> Bootstrap transport untuk membuktikan streaming architecture.

Bukan teknologi final.

---

# 8. Video Pipeline

Pipeline utama:

```text
Windows Virtual Display
        ↓
D3D11 Texture
        ↓
Frame Processor
        ↓
Hardware Video Encoder
        ↓
H.264 Stream
        ↓
USB TCP Tunnel
        ↓
Android Socket
        ↓
MediaCodec
        ↓
Surface
        ↓
Tablet Screen
```

---

# 9. Codec

Codec MVP:

```text
H.264 / AVC
```

Alasan:

* hardware encoder sangat luas di Windows
* hardware decoder luas di Android
* latency rendah
* implementasi relatif sederhana
* bandwidth cukup kecil untuk USB 2.0

Tidak menggunakan raw framebuffer.

Contoh raw 2560×1600:

```text
2560 × 1600 × 4 bytes × 60 FPS
≈ 983 MB/s
```

Jelas terlalu besar.

Dengan H.264:

```text
±10–30 Mbps
```

jauh lebih realistis.

---

# 10. Encoder

Windows harus menggunakan hardware accelerated encoding jika tersedia.

Priority:

```text
NVIDIA NVENC
Intel Quick Sync
AMD VCE/VCN
```

Untuk abstraction awal:

```text
Microsoft Media Foundation
```

diprioritaskan dibanding membuat integration vendor-specific.

Fallback:

```text
software encoder
```

boleh ada tetapi bukan prioritas.

---

# 11. Android Decoder

Android menggunakan:

```text
MediaCodec
```

Output diarahkan langsung ke:

```text
SurfaceView
```

atau:

```text
Surface
```

Hindari proses:

```text
decode → Bitmap → Canvas
```

karena menambah:

* latency
* CPU usage
* memory copy

Target pipeline:

```text
Network buffer
      ↓
MediaCodec
      ↓
Surface
```

sebisa mungkin zero-copy.

---

# 12. Latency Strategy

Latency merupakan KPI utama.

Pipeline tidak boleh menggunakan buffer video besar.

Target:

```text
Capture
< 5 ms

Encode
< 10 ms

USB transport
< 5 ms

Decode
< 10 ms

Render
< 16 ms
```

Target end-to-end:

```text
< 50 ms
```

Stretch target:

```text
< 35 ms
```

Goal akhirnya:

> Gerakan mouse di tablet terasa hampir langsung.

---

# 13. Buffer Strategy

Gunakan:

```text
latest-frame-wins
```

Jika decoder/transport tertinggal:

```text
Frame 100
Frame 101
Frame 102
Frame 103
```

dan tablet masih memproses frame 100:

jangan wajib memutar:

```text
101
102
103
```

Jika memungkinkan:

```text
drop old frames
→ render latest frame
```

Tujuan bukan perfect video playback.

Tujuannya:

> lowest interactive latency.

---

# 14. Frame Rate

MVP target:

```text
60 FPS
```

Fallback:

```text
30 FPS
```

apabila hardware tidak mampu.

UI host menampilkan:

```text
Resolution: 1920×1200
Refresh: 60 Hz
Streaming FPS: 60
Bitrate: 15 Mbps
Latency: 31 ms
```

---

# 15. Bitrate

Initial configuration:

### 1920×1200

```text
8–15 Mbps
```

### 2560×1600

```text
15–30 Mbps
```

Preset default:

```text
Quality:
○ Performance
● Balanced
○ Quality
```

MVP sebenarnya cukup expose satu:

```text
Balanced
```

Advanced control bisa ditambahkan kemudian.

---

# 16. Desktop Host

Recommended stack:

```text
C++
Windows Driver Kit
IddCx
Direct3D 11
Media Foundation
```

UI boleh menggunakan:

```text
Tauri
React
TypeScript
```

tetapi streaming engine jangan berada di JS.

Architecture:

```text
React UI
    ↓
Tauri
    ↓
Rust/native bridge
    ↓
Streaming Engine
    ↓
Driver
```

Untuk MVP ekstrem pertama, UI bahkan tidak wajib.

CLI acceptable:

```bash
second-display-host.exe
```

Output:

```text
[USB] Android device detected
[DISPLAY] Virtual monitor created
[STREAM] Encoder NVIDIA H264
[STREAM] 1920x1200 @ 60
[CLIENT] Connected
[LATENCY] 28 ms
```

---

# 17. Windows Components

## Component A

### Virtual Display Driver

```text
/driver
```

Technology:

```text
C++
WDK
IddCx
UMDF
```

Responsibilities:

* expose virtual monitor
* resolution modes
* refresh rate
* display lifecycle
* receive Windows display frames

---

## Component B

### Frame Pipeline

```text
/frame-pipeline
```

Responsibilities:

```text
receive frame
↓
GPU texture
↓
prepare encoder input
```

Target:

> minimum GPU → CPU copy.

---

## Component C

### Encoder

```text
/encoder
```

Responsibilities:

```text
H.264 hardware encode
bitrate management
keyframes
FPS
```

---

## Component D

### Transport

```text
/transport
```

MVP:

```text
TCP
through ADB reverse
```

Port:

```text
5000
```

Transport framing example:

```text
HEADER
payload_length
frame_id
timestamp
flags

PAYLOAD
H264 NAL data
```

---

# 18. Android Client

Recommended:

```text
Kotlin
Android Native
```

Tidak menggunakan Flutter untuk video renderer MVP.

Flutter boleh digunakan kelak untuk UI, tetapi critical rendering path tetap native.

Components:

```text
/android
    MainActivity
    ConnectionManager
    StreamReceiver
    VideoDecoder
    SurfaceRenderer
    StatsOverlay
```

---

# 19. Android Client UI

Screen awal:

```text
┌──────────────────────────┐
│       SecondScreen       │
│                          │
│ USB device detected      │
│                          │
│       CONNECT            │
│                          │
└──────────────────────────┘
```

Setelah connected:

```text
FULLSCREEN WINDOWS DISPLAY
```

Gesture Android system sebisa mungkin disembunyikan menggunakan immersive mode.

Overlay debug:

```text
60 FPS
23 ms
14 Mbps
1920×1200
```

Overlay bisa dimatikan.

---

# 20. Connection Protocol

Android melakukan:

```text
connect localhost:5000
```

Handshake:

```json
{
  "protocol": 1,
  "device": "Redmi Pad 2 Pro",
  "width": 2560,
  "height": 1600,
  "decoder": ["h264"]
}
```

Windows membalas:

```json
{
  "codec": "h264",
  "width": 1920,
  "height": 1200,
  "fps": 60,
  "bitrate": 12000000
}
```

Setelah handshake:

```text
video stream begins
```

---

# 21. Orientation

MVP wajib support:

```text
Landscape
```

Portrait bukan blocker untuk initial prototype.

Kemudian:

```text
Landscape
Portrait
```

Orientation tablet bisa dilaporkan ke Windows host.

Windows virtual monitor kemudian berubah:

```text
2560×1600
```

menjadi:

```text
1600×2560
```

---

# 22. Disconnect Behaviour

Jika:

```text
USB unplugged
```

maka:

```text
stream stops
```

Virtual monitor sebaiknya:

```text
disconnect / disabled
```

supaya window Windows tidak tertinggal di monitor virtual yang sudah tidak ada.

Reconnect flow:

```text
USB reconnect
↓
ADB reconnect
↓
client reconnect
↓
display active
```

---

# 23. MVP UX

Ideal development UX:

```bash
second-display dev
```

Command tersebut:

```text
1. Detect Android device
2. Run adb reverse
3. Start streaming host
4. Enable virtual display
5. Wait Android client
```

Misalnya output:

```text
SecondScreen Host v0.1

✓ Virtual display driver
✓ Redmi Pad 2 Pro detected
✓ USB tunnel established
✓ Android client connected
✓ H264 hardware encoder: NVIDIA

Streaming
1920×1200 @ 60 FPS
12.4 Mbps
Latency: 27 ms
```

---

# 24. Repository Structure

Recommended monorepo:

```text
second-screen/

├── apps/
│
│   ├── android/
│   │   ├── app/
│   │   └── build.gradle
│   │
│   └── desktop-ui/
│       ├── src/
│       └── src-tauri/
│
├── native/
│
│   ├── driver/
│   │   └── iddcx/
│   │
│   ├── streamer/
│   │
│   ├── encoder/
│   │
│   └── transport/
│
├── protocol/
│
├── scripts/
│   ├── install-driver.ps1
│   ├── adb-connect.ps1
│   └── dev.ps1
│
├── docs/
│
└── README.md
```

---

# 25. Development Milestones

## M0 — Android Video Prototype

Goal:

Prove Android mampu menerima low-latency H.264 stream.

Tidak ada virtual display.

Input:

```text
sample/generated Windows frames
```

Flow:

```text
Windows test video
↓
H264
↓
ADB USB
↓
Android
```

Success:

```text
1080p
60 FPS
stable
```

---

# 26. M1 — Virtual Monitor

Implement IddCx driver.

Windows harus memperlihatkan:

```text
Display 2
```

Success criteria:

```text
Windows Settings detects second display
```

dan:

```text
Extend Desktop
```

bisa dipilih.

Belum perlu streaming Android.

---

# 27. M2 — Virtual Display → Android

Gabungkan:

```text
IddCx
↓
Frame Pipeline
↓
Encoder
↓
ADB
↓
Android
```

Target:

```text
1920×1200
60 FPS
```

Success criteria:

User bisa:

```text
drag Notepad
```

dari monitor PC menuju tablet.

Notepad kemudian terlihat di tablet.

Ini milestone MVP terpenting.

---

# 28. M3 — Latency Optimization

Measure:

```text
capture latency
encode latency
transport latency
decode latency
render latency
```

Add frame timestamp.

Contoh:

```text
Frame 8192

capture     0 ms
encode      6 ms
USB         2 ms
decode      5 ms
render      8 ms

total:
21 ms
```

Goal:

```text
< 50 ms
```

---

# 29. M4 — Native Resolution

Enable:

```text
2560×1600 @ 60
```

Test on Redmi Pad 2 Pro.

Measure:

```text
FPS
latency
bitrate
GPU usage
decoder stability
temperature
```

---

# 30. M5 — Reconnect & Stability

Test:

```text
USB unplug
USB reconnect
tablet app restart
Windows sleep
tablet sleep
screen rotation
host restart
```

Minimum stability test:

```text
2 hours continuous usage
```

Target:

```text
no crash
no memory leak
no progressive latency
```

---

# 31. MVP Acceptance Criteria

MVP dianggap berhasil jika:

### Display

* Windows mendeteksi Android sebagai Display 2.
* Extend Desktop bekerja.
* Window bisa dipindahkan ke tablet.

### Connection

* video dikirim melalui USB.
* tidak memerlukan Wi-Fi.
* reconnect dasar bekerja.

### Performance

1920×1200:

```text
60 FPS
```

Target latency:

```text
< 50 ms
```

### Stability

Streaming:

```text
≥ 2 hours
```

tanpa:

```text
crash
major frame corruption
memory leak signifikan
latency terus bertambah
```

---

# 32. Explicitly Out of Scope

MVP V0.1 tidak mengerjakan:

```text
❌ macOS host
❌ Linux host

❌ iPad / iOS

❌ Wi-Fi streaming

❌ touchscreen input
❌ stylus
❌ pressure sensitivity

❌ audio streaming

❌ clipboard sync

❌ file transfer

❌ multi-monitor Android

❌ 120 Hz

❌ HDR

❌ DRM video optimization

❌ commercial driver signing
```

---

# 33. V0.2

Setelah V0.1 stabil:

### Primary objectives

```text
Native USB Transport
Touch Input
Auto Connect
```

Hilangkan dependency:

```text
ADB
USB debugging
Developer Options
```

Touch path:

```text
Android touch
↓
protocol
↓
Windows host
↓
Windows input injection
```

Features:

* tap
* click
* drag
* scroll
* right click
* basic multitouch

---

# 34. V0.3

Target:

```text
Polish
```

Features:

```text
HEVC
Dynamic bitrate
Dynamic resolution
Auto orientation
Stylus
Pressure
Clipboard
Better reconnect
```

---

# 35. V0.4

Experimental:

```text
120 Hz
Wi-Fi
Multiple tablets
AV1
```

Wi-Fi baru dimasukkan setelah USB solid.

Reason:

> USB adalah core experience produk.

---

# 36. Technical Risks

## Risk 1 — Virtual Display Driver

Ini bagian paling kompleks.

Potential problems:

```text
driver lifecycle
D3D swapchain
Windows updates
resolution changes
driver signing
```

Mitigation:

Mulai dari Microsoft IddCx sample.

Jangan membuat driver architecture dari nol.

---

## Risk 2 — GPU Copy

Jika pipeline:

```text
GPU
↓
CPU
↓
GPU encoder
```

latency dan bandwidth memory meningkat.

Target architecture:

```text
D3D Texture
↓
Hardware Encoder
```

dengan copy seminimal mungkin.

---

## Risk 3 — Encoder Buffering

Hardware encoder sering optimize untuk video playback.

Kita membutuhkan:

```text
low latency mode
```

Hindari:

```text
B-frames
large GOP buffering
lookahead
```

Prefer:

```text
low-delay
real-time
minimal buffering
```

---

# 37. Development Principles

Prioritas development harus:

```text
LATENCY
   ↓
STABILITY
   ↓
FPS
   ↓
IMAGE QUALITY
   ↓
FEATURES
```

Jangan kebalik menjadi:

```text
UI
Touch
Settings
Fancy animation
```

sebelum display pipeline stabil.

---

# 38. First Implementation Target

Target pertama bukan membuat aplikasi lengkap.

Target pertama:

> Membuat sebuah window Windows muncul di layar tablet melalui virtual extended display.

Test scenario:

```text
PC Monitor
Display 1

Redmi Pad 2 Pro
Display 2
```

Lalu:

```text
Open Notepad
↓
drag ke kanan
↓
Notepad keluar dari monitor PC
↓
Notepad muncul di Redmi Pad
```

Dengan:

```text
USB only
60 FPS
< 50 ms latency
```

Jika skenario ini berhasil, fundamental product sudah terbukti.

---

# 39. Recommended Build Order

Implement dalam urutan:

```text
1. Android H264 decoder
        ↓
2. USB ADB transport
        ↓
3. Windows H264 test streamer
        ↓
4. Measure latency
        ↓
5. IddCx virtual display
        ↓
6. Feed display frames to encoder
        ↓
7. Combine entire pipeline
        ↓
8. Optimize latency
        ↓
9. Native 2560×1600
        ↓
10. Reconnect/stability
```

**Jangan mulai dari UI desktop.**

UI dibuat setelah:

```text
Windows virtual display
        ↓
USB
        ↓
Android screen
```

sudah bekerja.

---

# 40. Definition of Done — V0.1

V0.1 selesai ketika:

```text
Windows 11
+
Redmi Pad 2 Pro
+
USB cable
```

dapat digunakan selama aktivitas normal seperti:

```text
VS Code
Terminal
Browser
ChatGPT
Discord
Monitoring dashboard
```

dengan tablet sebagai:

> **real extended second display**

pada:

```text
1920×1200 atau lebih
60 FPS
<50 ms target latency
```

tanpa menggunakan jaringan Wi-Fi.
