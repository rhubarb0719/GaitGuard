"""
GSTRIDE v3 -> Edge Impulse preprocessing script
===============================================
Reads calibrated IMU CSVs from GSTRIDE v3, windows the signals, and exports per-window CSVs
ready for Edge Impulse upload. Splits train/test by subject (no leakage).

Usage:
    # Dry-run (no file writing, print stats only)
    python preprocess_gstride.py --dry-run
    # Write files (clears OUTPUT_DIR/{training,testing} first)
    python preprocess_gstride.py --write

Expected layout:
    project_root/
        preprocess_gstride.py          <- 本脚本
        GSTRIDE_DDBB.xlsx
        GSTRIDE_database/              <- extracted dataset
            _IMU/
                V001.csv, V002.csv, ...
        ei_dataset_5s/                 <- this script creates it
            training/
                faller.V001_win000.csv
                nonfaller.V003_win000.csv
                ...
            testing/
                faller.V099_win000.csv
                ...
"""

import argparse
import pandas as pd
import numpy as np
from pathlib import Path
from sklearn.model_selection import train_test_split

# ========== Parameters ==========
DATASET_ROOT = Path("GSTRIDE_database")          # dataset root
EXCEL_FILE = Path("GSTRIDE_DDBB.xlsx")           # labels Excel (v3)
IMU_DIR = None                                   # auto-detect (IMU or _IMU)
OUTPUT_DIR = Path("ei_dataset_5s")               # output root (5s windows)

WINDOW_SEC = 5.0        # window length (seconds)
STRIDE_SEC = 5.0        # stride (seconds), no overlap
SAMPLING_HZ = 103.42    # nominal sampling rate for EI timestamps
TEST_RATIO = 0.2        # subject-level test split
RANDOM_SEED = 42        # reproducibility
TRIM_HEAD_S = 2.0       # trim first seconds (transient)
TRIM_TAIL_S = 2.0       # trim last seconds (transient)
USE_CSIC_ONLY = True    # keep CSIC device subjects only
DROP_MISSING_IMU = True # skip subjects with missing IMU CSV (warn)
MAX_WINDOWS_PER_SUBJECT = 150

SHEET_NAME = "GSTRIDE_DDBB"
ID_COL = "ID (VNNN)"
FALLER_COL = "Falls during the last year - prior to test (YES/NO)"
DEVICE_COL = "Type of device (CSIC/Gaitup)"

IMU_REQUIRED_COLS = [
    "accX[m/s^2]",
    "accY[m/s^2]",
    "accZ[m/s^2]",
    "gyrX[rad/s]",
    "gyrY[rad/s]",
    "gyrZ[rad/s]",
    "timestamp[s]",
]


def normalize_col_name(name):
    """Normalize Excel column names (newlines / extra spaces)."""
    return " ".join(str(name).replace("\n", " ").split())


def find_column(df, expected_name, keywords):
    """Find a column by exact name or keyword fallback."""
    normalized = {normalize_col_name(c): c for c in df.columns}
    expected_norm = normalize_col_name(expected_name)
    if expected_norm in normalized:
        return normalized[expected_norm]

    matches = []
    for norm_name, original_name in normalized.items():
        norm_lower = norm_name.lower()
        if all(keyword.lower() in norm_lower for keyword in keywords):
            matches.append(original_name)

    if len(matches) == 1:
        return matches[0]
    if not matches:
        raise KeyError(f"Missing column: {expected_name}")
    raise KeyError(f"Ambiguous column match: {expected_name} -> {matches}")


def check_input_paths():
    """Validate input paths and auto-detect IMU directory."""
    global IMU_DIR
    print(f"[path] DATASET_ROOT: {DATASET_ROOT.resolve()}")
    print(f"[path] EXCEL_FILE:   {EXCEL_FILE.resolve()}")
    if not EXCEL_FILE.exists():
        raise FileNotFoundError(f"Excel file not found: {EXCEL_FILE}")
    if not DATASET_ROOT.exists():
        raise FileNotFoundError(f"Dataset root not found: {DATASET_ROOT}")

    candidates = [DATASET_ROOT / "_IMU", DATASET_ROOT / "IMU"]
    existing = [p for p in candidates if p.exists()]
    if not existing:
        raise FileNotFoundError(f"IMU directory not found (tried {candidates})")
    if len(existing) > 1:
        print(f"[warn] Multiple IMU directories found, using: {existing[0].resolve()}")
    IMU_DIR = existing[0]
    print(f"[path] IMU_DIR:      {IMU_DIR.resolve()}")


def load_labels():
    """Read Excel and return {V001: 'faller'/'nonfaller'}. Keeps CSIC only if enabled."""
    xl = pd.ExcelFile(EXCEL_FILE)
    print(f"[excel] sheets: {xl.sheet_names}")

    df = pd.read_excel(EXCEL_FILE, sheet_name=SHEET_NAME, header=1)
    df.columns = [normalize_col_name(c) for c in df.columns]

    id_col = find_column(df, ID_COL, ["ID", "VNNN"])
    faller_col = find_column(df, FALLER_COL, ["Falls", "last year", "YES/NO"])
    device_col = find_column(df, DEVICE_COL, ["Type of device", "CSIC", "Gaitup"])
    print(f"[excel] ID column: {id_col}")
    print(f"[excel] Falls label column: {faller_col}")
    print(f"[excel] Device column: {device_col}")

    # Keep only valid subject rows (skip legend rows, etc.)
    df = df[df[id_col].astype(str).str.match(r'^V\d{3}$', na=False)].copy()
    print(f"[excel] Valid subjects in sheet: {len(df)}")

    # Clean labels (strip and uppercase)
    df['faller_raw'] = df[faller_col].astype(str).str.strip().str.upper()
    df['device'] = df[device_col].astype(str).str.strip()
    print("[excel] YES/NO distribution (raw):")
    print(df['faller_raw'].value_counts(dropna=False).to_string())
    print("[excel] Device distribution:")
    print(df['device'].value_counts(dropna=False).to_string())

    if USE_CSIC_ONLY:
        df = df[df['device'].str.upper() == 'CSIC'].copy()
        print(f"[filter] Keep CSIC only: {len(df)} subjects")

    df['label'] = df['faller_raw'].map({'YES': 'faller', 'NO': 'nonfaller'})
    bad_labels = df[df['label'].isna()][[id_col, faller_col, 'faller_raw']]
    if not bad_labels.empty:
        print("[warn] Unmappable labels found:")
        print(bad_labels.to_string(index=False))
    df = df.dropna(subset=['label'])

    labels = dict(zip(df[id_col], df['label']))
    faller_count = sum(v == 'faller' for v in labels.values())
    nonfaller_count = sum(v == 'nonfaller' for v in labels.values())
    print(f"[labels] faller={faller_count}, nonfaller={nonfaller_count}")
    return labels


def filter_labels_with_existing_imu(labels):
    """Drop subjects with labels but missing IMU CSVs (warn loudly)."""
    missing = [subject_id for subject_id in labels
               if not (IMU_DIR / f"{subject_id}.csv").exists()]
    if missing:
        missing_counts = {
            'faller': sum(labels[subject_id] == 'faller' for subject_id in missing),
            'nonfaller': sum(labels[subject_id] == 'nonfaller' for subject_id in missing),
        }
        print(f"[imu] {len(missing)} labeled subjects missing IMU CSV: {', '.join(missing)}")
        print(f"[imu] Missing-label distribution: faller={missing_counts['faller']}, "
              f"nonfaller={missing_counts['nonfaller']}")
        if not DROP_MISSING_IMU:
            raise FileNotFoundError("Missing IMU CSVs detected; stopping.")
        labels = {subject_id: label for subject_id, label in labels.items()
                  if subject_id not in missing}
        print("[imu] Missing subjects removed; continuing.")

    faller_count = sum(v == 'faller' for v in labels.values())
    nonfaller_count = sum(v == 'nonfaller' for v in labels.values())
    print(f"[imu] Subjects with IMU CSVs: {len(labels)} "
          f"(faller={faller_count}, nonfaller={nonfaller_count})")
    return labels


def split_subjects(labels, test_ratio=TEST_RATIO, seed=RANDOM_SEED):
    """Subject-level train/test split with stratification."""
    ids = list(labels.keys())
    y = [labels[i] for i in ids]
    train_ids, test_ids = train_test_split(
        ids, test_size=test_ratio, stratify=y, random_state=seed
    )
    print(f"[split] train subjects={len(train_ids)}, test subjects={len(test_ids)}")
    return set(train_ids), set(test_ids)


def _pick_max_windows_uniform(windows, max_windows):
    if len(windows) <= max_windows:
        return windows
    idx = np.linspace(0, len(windows) - 1, max_windows)
    idx = np.unique(np.round(idx).astype(int))
    if len(idx) < max_windows:
        all_idx = np.arange(len(windows))
        remaining = np.setdiff1d(all_idx, idx)
        missing = max_windows - len(idx)
        extra = np.linspace(0, len(remaining) - 1, missing)
        extra = np.unique(np.round(extra).astype(int))
        idx = np.sort(np.concatenate([idx, remaining[extra][:missing]]))
    return [windows[i] for i in idx[:max_windows]]


def window_signal(df_imu, win_samples, stride_samples, trim_head_s, trim_tail_s):
    """
    把一个受试者的 IMU DataFrame 切成多个窗口。
    返回 list of DataFrame (每个 win_samples 行, 6 列 + ms timestamp)。
    """
    missing_cols = [c for c in IMU_REQUIRED_COLS if c not in df_imu.columns]
    if missing_cols:
        raise KeyError(f"IMU CSV missing required columns: {missing_cols}")

    ts = pd.to_numeric(df_imu["timestamp[s]"], errors="coerce")
    if ts.isna().all():
        raise ValueError("timestamp[s] is all NaN; cannot trim by time.")
    t0 = float(ts.min())
    t1 = float(ts.max())
    keep = (ts >= (t0 + float(trim_head_s))) & (ts <= (t1 - float(trim_tail_s)))
    df_imu = df_imu.loc[keep].reset_index(drop=True)

    cols_6 = [
        "accX[m/s^2]",
        "accY[m/s^2]",
        "accZ[m/s^2]",
        "gyrX[rad/s]",
        "gyrY[rad/s]",
        "gyrZ[rad/s]",
    ]

    windows = []
    n = len(df_imu)
    start = 0
    while start + win_samples <= n:
        chunk = df_imu.iloc[start:start + win_samples][cols_6].reset_index(drop=True)
        # Create a clean ms timestamp starting at 0 for Edge Impulse
        ts_ms = (np.arange(win_samples) * (1000.0 / SAMPLING_HZ)).round(2)
        out = pd.DataFrame({
            'timestamp': ts_ms,
            'accX': chunk['accX[m/s^2]'].values,
            'accY': chunk['accY[m/s^2]'].values,
            'accZ': chunk['accZ[m/s^2]'].values,
            'gyrX': chunk['gyrX[rad/s]'].values,
            'gyrY': chunk['gyrY[rad/s]'].values,
            'gyrZ': chunk['gyrZ[rad/s]'].values,
        })
        windows.append(out)
        start += stride_samples
    windows = _pick_max_windows_uniform(windows, MAX_WINDOWS_PER_SUBJECT)
    return windows


def process_subject(
    subject_id,
    label,
    out_dir,
    win_samples,
    stride_samples,
    trim_head_s,
    trim_tail_s,
    *,
    write_files,
):
    """Process one subject: read CSV -> window -> (optional) write. Returns window count."""
    csv_path = IMU_DIR / f"{subject_id}.csv"
    if not csv_path.exists():
        raise FileNotFoundError(f"IMU CSV not found: {csv_path}")
    df_imu = pd.read_csv(csv_path)
    wins = window_signal(df_imu, win_samples, stride_samples, trim_head_s, trim_tail_s)
    if not wins:
        raise ValueError(f"{subject_id} produced 0 windows; check CSV length or parameters.")
    if write_files:
        for i, w in enumerate(wins, start=1):
            out_path = out_dir / f"{label}.{subject_id}_win{i:03d}.csv"
            w.to_csv(out_path, index=False)
    return len(wins)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dry-run", action="store_true", help="Print stats only, do not write files")
    parser.add_argument("--write", action="store_true", help="Clear OUTPUT_DIR and write CSV windows")
    args = parser.parse_args()
    if args.dry_run and args.write:
        raise ValueError("--dry-run and --write are mutually exclusive")
    if not args.dry_run and not args.write:
        args.dry_run = True

    check_input_paths()

    # 1) Load labels
    labels = load_labels()
    labels = filter_labels_with_existing_imu(labels)

    # 2) Subject-level split
    train_ids, test_ids = split_subjects(labels)

    # 3) Output dirs (cleared only in --write mode)
    train_dir = OUTPUT_DIR / "training"
    test_dir  = OUTPUT_DIR / "testing"
    if args.write:
        train_dir.mkdir(parents=True, exist_ok=True)
        test_dir.mkdir(parents=True, exist_ok=True)
        for folder in (train_dir, test_dir):
            for old_csv in folder.glob("*.csv"):
                old_csv.unlink()
        print(f"[output] Cleared old CSVs: {train_dir}, {test_dir}")
    else:
        print("[output] Dry-run mode: no files will be deleted or written")

    # 4) Window parameters (samples)
    win_samples    = int(round(WINDOW_SEC * SAMPLING_HZ))   # ≈ 207
    stride_samples = int(round(STRIDE_SEC * SAMPLING_HZ))   # ≈ 207
    print(f"[window] SAMPLING_HZ={SAMPLING_HZ}")
    print(f"[window] win_samples={win_samples}, stride_samples={stride_samples} (must be > 0)")
    if win_samples <= 0 or stride_samples <= 0:
        raise ValueError("win_samples/stride_samples <= 0; check WINDOW_SEC/STRIDE_SEC/SAMPLING_HZ")
    print(f"[window] Trim transients: head {TRIM_HEAD_S}s / tail {TRIM_TAIL_S}s (by timestamp[s])")

    # 5) Process subjects
    stats = {'train': {'faller': 0, 'nonfaller': 0},
             'test':  {'faller': 0, 'nonfaller': 0}}
    file_count = 0
    for subject_id in sorted(labels.keys()):
        label = labels[subject_id]
        is_train = subject_id in train_ids
        out_dir = train_dir if is_train else test_dir
        n = process_subject(
            subject_id,
            label,
            out_dir,
            win_samples,
            stride_samples,
            TRIM_HEAD_S,
            TRIM_TAIL_S,
            write_files=args.write,
        )
        stats['train' if is_train else 'test'][label] += n
        if n <= 0:
            raise RuntimeError(f"{subject_id} has IMU CSV but produced 0 windows; stopping.")
        if n > MAX_WINDOWS_PER_SUBJECT:
            raise RuntimeError(f"{subject_id} windows={n} exceeds MAX_WINDOWS_PER_SUBJECT={MAX_WINDOWS_PER_SUBJECT}")
        file_count += n
        print(f"{subject_id} [{label}] -> {n} windows ({'train' if is_train else 'test'})")

    # 6) Summary
    print("\n========== DONE ==========")
    print(f"Training: faller={stats['train']['faller']}, nonfaller={stats['train']['nonfaller']}")
    print(f"Testing:  faller={stats['test']['faller']}, nonfaller={stats['test']['nonfaller']}")
    print(f"Total windows/files: {file_count}")
    if args.dry_run:
        print("\n[hint] If stats look good, write files with: python preprocess_gstride.py --write")
    print(f"\nOutput directory: {OUTPUT_DIR.resolve()}")
    print("\nNext: upload via edge-impulse-uploader (two categories)")
    print(f"  edge-impulse-uploader --category training {train_dir}/*.csv")
    print(f"  edge-impulse-uploader --category testing  {test_dir}/*.csv")


if __name__ == "__main__":
    main()
