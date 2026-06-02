# ML Pipeline (GSTRIDE → Edge Impulse)

This folder contains the **ML data preprocessing pipeline** used to convert the GSTRIDE v3 dataset into windowed CSV files that can be uploaded to **Edge Impulse**, plus (optionally) the training results artifacts.

---

## What’s in this folder

```
ml/
  preprocess_gstride.py      # GSTRIDE v3 → Edge Impulse preprocessing script
  GSTRIDE_DDBB.xlsx          # labels Excel (input)
  GSTRIDE_database/          # extracted dataset root (input)
    _IMU/                    # subject IMU CSVs: V001.csv, V002.csv, ...
  ei_dataset_5s/             # generated output dataset for Edge Impulse upload
    training/
    testing/
  (optional) ei_project_link.md
  (optional) results/
```

---

## Goal

- Convert each subject’s calibrated IMU time series into **fixed-length windows**
- Export each window as a CSV file for Edge Impulse
- Split **train/test by subject** (subject-level split) to avoid leakage

The script produces two folders:
- `ei_dataset_5s/training/`
- `ei_dataset_5s/testing/`

Each file is named like:
`{label}.{subject_id}_win###.csv`  
Example: `faller.V001_win001.csv`

---

## Inputs

### Labels
- **File**: `GSTRIDE_DDBB.xlsx`
- **Used columns (conceptually)**:
  - Subject ID (e.g., `V001`)
  - Fall history YES/NO → mapped to class labels:
    - `YES` → `faller`
    - `NO` → `nonfaller`
  - Device type (optionally filtered to a specific device type)

### IMU CSVs
- **Folder**: `GSTRIDE_database/_IMU/`
- **Expected columns** (from the script):
  - `accX[m/s^2]`, `accY[m/s^2]`, `accZ[m/s^2]`
  - `gyrX[rad/s]`, `gyrY[rad/s]`, `gyrZ[rad/s]`
  - `timestamp[s]`

---

## How to run preprocessing

Run from the `ml/` directory.

### 1) Dry-run (recommended first)

This prints dataset stats and verifies paths without writing files:

```bash
python preprocess_gstride.py --dry-run
```

### 2) Write output CSV windows

This clears old files under `ei_dataset_5s/{training,testing}` and regenerates them:

```bash
python preprocess_gstride.py --write
```

---

## Key parameters (current script defaults)

These are defined near the top of `preprocess_gstride.py`:

- **Window length**: 5.0 seconds
- **Stride**: 5.0 seconds (no overlap)
- **Nominal sampling rate (for Edge Impulse timestamps)**: 103.42 Hz
- **Transient trim**: 2.0 seconds at the start and end (by `timestamp[s]`)
- **Train/test split**: 80/20, **by subject** (stratified)
- **Per-subject max windows**: 150 (uniformly sampled if more exist)

---

## Upload to Edge Impulse

After running `--write`, upload by category:

```bash
edge-impulse-uploader --category training ei_dataset_5s/training/*.csv
edge-impulse-uploader --category testing  ei_dataset_5s/testing/*.csv
```

If you don’t have the CLI installed, see Edge Impulse docs for installing `edge-impulse-uploader`.

---

## Edge Impulse project link (required if your course asks for it)

Create `ml/ei_project_link.md` and paste your **public Edge Impulse project link** there.

Suggested contents:
- Public project URL
- Notes on the final DSP/feature block(s) and classifier settings

---

## Results artifacts (recommended for grading/review)

If you need to submit evidence of experiments, add:

```
ml/results/
  feature_explorer_*.png
  confusion_matrix_*.png
  comparison_table.md
```

At minimum, include:
- The **final** Feature Explorer screenshot
- The **final** confusion matrix screenshot
- A short comparison table describing iterations (e.g., 2s vs 5s, FFT vs Wavelet, metrics)

