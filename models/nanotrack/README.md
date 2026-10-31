NanoTrack v2 models are from [HonglinChu/SiamTrackers](https://github.com/HonglinChu/SiamTrackers/tree/248663fde6bf7c40190cf10ee396d5662919ecd3/NanoTrack/models/nanotrackv2), the model family linked by [OpenCV's TrackerNano documentation](https://docs.opencv.org/4.x/d8/d69/classcv_1_1TrackerNano.html).

`provenance.json` records the exact revision, download URLs, file sizes, and SHA-256 hashes. `UPSTREAM_LICENSE` is copied from the upstream `NanoTrack/LICENSE`. Restore or verify the files using `python3 tools/download_models.py`.

The local inference decoder in `precision_tracker.py` uses the NanoTrack v2 tensor layout and search/box decoding described by the original NanoTrack implementation and [OpenCV's implementation](https://github.com/opencv/opencv/blob/4.10.0/modules/video/src/tracking/tracker_nano.cpp). The reference template is preserved. Response confidence excludes the artificial Hann position preference.
