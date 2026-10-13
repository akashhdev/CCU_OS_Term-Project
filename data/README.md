# CWRU bearing dataset

`prepare_cwru.py` downloads the four CWRU 12 kHz drive-end recordings used by the
project and cuts them into 1024-sample frames (WBS P3.1).

| Class | id | File | Variable | Derating setpoint |
| --- | --- | --- | --- | --- |
| normal | 0 | `97.mat` | `X097_DE_time` | 100 % |
| ball fault | 1 | `118.mat` | `X118_DE_time` | 85 % |
| inner race fault | 2 | `105.mat` | `X105_DE_time` | 70 % |
| outer race fault | 3 | `130.mat` | `X130_DE_time` | 60 % |

```sh
pip install -r data/requirements.txt
python3 data/prepare_cwru.py --download        # fetches into data/raw/cwru/, writes data/processed/
```

If the CWRU site can't be reached, copy the four `.mat` files into
`data/raw/cwru/` by hand, for example from the Kaggle mirror
`brnithish/cwru-bearing-dataset`, and run the script without `--download`.

## Outputs (`data/processed/`, git-ignored)

- `cwru_windows.npz` holds `X_train`, `X_val` and `X_test` as float32 arrays of shape
  (N, 1024), the matching `y_*` labels, and `classes`, `sample_rate` and `frame_len`.
  Use it to train the 1D-CNN (P3.2).
- `frames.f32` holds the test frames as raw little-endian float32, in blocks of 16
  frames per class. It is the replay input for the Linux analytics thread:
  `mpu_linux/workload/pdm_bench --frames data/processed/frames.f32`.
- `frames_labels.csv` gives the ground-truth class of each frame in `frames.f32`.

## Avoiding train/test leakage

Each recording is split in time (70 / 15 / 15 %) **before** windowing. Training
windows overlap (hop 512, set with `--train-stride`) to get more samples.
Validation and test windows don't overlap, and no window crosses a split
boundary. Splitting overlapping windows at random would put near-duplicate
frames in both train and test, which inflates the test accuracy.
