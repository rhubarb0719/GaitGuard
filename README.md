# GaitGuard (Individual Project Deliverables)

This repository contains the main deliverables for a prototype **wearable fall-risk detection / alerting** project: enclosure hardware design, on-device inference firmware, and an ML data preprocessing + Edge Impulse training workflow based on the GSTRIDE dataset.

> Designed for review: start from the architecture below, then jump to each deliverable folder.

---

## Poster

[![GaitGuard poster](docs/HWSW2-poster.jpg)](docs/HWSW2-poster.pdf)

Click the image to open the full-resolution PDF: [`docs/HWSW2-poster.pdf`](docs/HWSW2-poster.pdf)

---

## Architecture (end-to-end)

The system consists of two major parts: an **offline ML pipeline** (data → model) and an **on-device runtime** (sensor → inference → feedback).

```
 OFFLINE (ML)                                  DEVICE (Firmware, XIAO ESP32-S3)
 ============                                  ================================

 +-------------------------------+             +-------------------------------+
 | GSTRIDE v3 dataset            |             | LSM6DS3 IMU (6-axis, I2C)     |
 | IMU CSVs + labels Excel       |             | 104 Hz, +-16 g / +-2000 dps   |
 +---------------+---------------+             +---------------+---------------+
                 |                                             |
                 v                                             v
 +-------------------------------+             +-------------------------------+
 | Preprocess & windowing        |             | Window buffer (5 s)           |
 | ml/preprocess_gstride.py      |             | firmware/src/main.cpp         |
 +---------------+---------------+             +---------------+---------------+
                 |                                             |
                 v                                             v
 +-------------------------------+             +-------------------------------+
 | Edge Impulse dataset          |             | Static check                  |
 | training / testing CSVs       |             | not walking -> stay silent    |
 +---------------+---------------+             +---------------+---------------+
                 |                                             |
                 v                                             v
 +-------------------------------+             +-------------------------------+
 | Edge Impulse training         |             | CNN inference                 |
 | multi-scale 1D CNN (INT8)     |             | run_classifier() -> P(faller) |
 +---------------+---------------+             +---------------+---------------+
                 |                                             |
                 v                                             v
 +-------------------------------+             +-------------------------------+
 | Exported inference library    |             | Two-layer smoothing           |
 | firmware/lib/                 | ----------> | 5-window avg -> 6-window risk |
 |   a5s-Gaitgaurd_inferencing/  |  compiled   +---------------+---------------+
 +-------------------------------+  into the                   |
                                    firmware                   v
                                               +-------------------------------+
                                               | Feedback                      |
                                               | LOW    < 0.55      silent     |
                                               | MEDIUM 0.55-0.75   vibration  |
                                               | HIGH   > 0.75      buzzer     |
                                               +-------------------------------+
```

---

## Deliverables map (annotated tree)

```
GaitGuard/
├── README.md                          ← Repository front page (this file)
├── hardware/                          ← Enclosure / hardware documentation
│   ├── front layer.png                ← Layer diagram (front)
│   ├── back layer.png                 ← Layer diagram (back)
│   ├── printed enclosure.JPG          ← Photo of the printed enclosure
│   └── enclosure/                     ← 3D printable files
│       ├── 底部.stl
│       ├── 盖子.stl
│       └── 扁嘴夹.3mf
├── firmware/                          ← ESP32-S3 firmware (PlatformIO)
│   ├── platformio.ini                 ← Board/framework/serial settings
│   ├── src/
│   │   └── main.cpp                   ← Main firmware: IMU → inference → feedback
│   └── lib/
│       └── a5s-Gaitgaurd_inferencing/ ← Edge Impulse exported inference library
├── ml/                                ← ML preprocessing + Edge Impulse dataset export
│   ├── README.md                      ← ML pipeline documentation
│   └── preprocess_gstride.py          ← GSTRIDE → windowed CSVs for Edge Impulse
└── docs/                              ← Poster
    ├── HWSW2-poster.pdf               ← Project poster
    └── HWSW2-poster.jpg               ← Poster preview image (shown in this README)
```

---

## Hardware (`hardware/`)

Goal: provide enclosure/structure design assets and assembly references.

### Key files

- `hardware/front layer.png`, `hardware/back layer.png`: layered structure diagrams.
- `hardware/printed enclosure.JPG`: photo of the printed enclosure / assembly.
- `hardware/enclosure/`: 3D enclosure & clip files (`.stl` / `.3mf`).

---

## Firmware (`firmware/`)

Goal: read **LSM6DS3 6-axis IMU** data on a **Seeed XIAO ESP32S3**, buffer samples at `EI_CLASSIFIER_FREQUENCY`, run inference, then map `P(faller)` to alert policies (silent / vibration / buzzer).

### Key files

- `firmware/platformio.ini`: PlatformIO project config (board, framework, serial baud rate, etc.).
- `firmware/src/main.cpp`: main application
  - Reads IMU via I2C (acceleration in m/s², angular velocity in rad/s)
  - Sampling frequency is `EI_CLASSIFIER_FREQUENCY`
  - Runs `run_classifier(...)` whenever a full window is collected
  - Applies a sliding average over the latest 3 windows for the `faller` probability, then triggers different feedback policies
- `firmware/lib/a5s-Gaitgaurd_inferencing/`: Edge Impulse exported inference library (linked and used by `main.cpp`)

### Quick start (local development)

1. Install PlatformIO (VSCode or CLI).
2. Go to `firmware/`, build, and flash to the XIAO ESP32S3.
3. Open the serial monitor at `115200` (see `platformio.ini`). You should see:
   - Raw 6-axis debug output (about once per second)
   - `P(faller)` and the smoothed probability
   - Feedback level (LOW/MEDIUM/HIGH)

> Note: IMU full-scale settings and unit conversions are aligned in `firmware/src/main.cpp` to match the training data units. If you change the sensor or its range, adjust this section accordingly.

---

## ML (`ml/`)

Goal: preprocess **GSTRIDE v3** subject IMU CSVs + label Excel into per-window CSV files that can be uploaded to Edge Impulse (train/test split by subject to avoid leakage).

See `ml/README.md` for the full pipeline documentation and exact commands:
- Preprocessing: `python preprocess_gstride.py --dry-run` / `--write`
- Edge Impulse upload commands
- Parameter defaults (window/stride/split/trim)

> The GSTRIDE v3 dataset (subject CSVs and the `GSTRIDE_DDBB.xlsx` labels file) is not included in this repo. Download it separately and place it in `ml/` before running the script.

---

## Notes (recommended cleanup before submission)

This repo may include `.DS_Store` files (generated by macOS Finder). If your submission rules disallow them, delete them before submitting.

Large artifacts are intentionally ignored in `.gitignore` (e.g., datasets) to keep the GitHub repo lightweight.

