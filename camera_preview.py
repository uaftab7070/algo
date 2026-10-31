#!/usr/bin/env python3
"""Live laptop-camera preview of the project's template/LK single-object tracker."""
import time

import cv2
import numpy as np


def clamp01(value):
    return float(max(0.0, min(1.0, value)))


def main():
    camera = cv2.VideoCapture(0, cv2.CAP_AVFOUNDATION)
    if not camera.isOpened():
        raise SystemExit("Could not open camera 0. Check macOS camera permission for Terminal/Python.")
    camera.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
    camera.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
    ok, frame = camera.read()
    if not ok:
        camera.release()
        raise SystemExit("Camera opened, but no frame was received.")

    roi = cv2.selectROI("Select target — drag a box, then press Enter", frame, False, False)
    cv2.destroyWindow("Select target — drag a box, then press Enter")
    x, y, w, h = map(int, roi)
    if w < 4 or h < 4:
        camera.release()
        raise SystemExit("No valid ROI selected.")

    prev = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    box = np.array([x, y, w, h], dtype=np.float32)
    last_valid = box.copy()
    initial = prev[y:y+h, x:x+w].copy()
    shrink = min(1.0, 256.0 / max(initial.shape[:2]))
    if shrink < 1.0:
        initial = cv2.resize(initial, None, fx=shrink, fy=shrink, interpolation=cv2.INTER_AREA)
    short = initial.copy()
    kalman = cv2.KalmanFilter(8, 4)
    kalman.transitionMatrix = np.array([
        [1,0,1,0,0,0,0,0], [0,1,0,1,0,0,0,0], [0,0,1,0,0,0,0,0], [0,0,0,1,0,0,0,0],
        [0,0,0,0,1,0,0,0], [0,0,0,0,0,1,0,0], [0,0,0,0,0,0,1,0], [0,0,0,0,0,0,0,1]
    ], dtype=np.float32)
    kalman.measurementMatrix = np.zeros((4, 8), np.float32)
    kalman.measurementMatrix[0, 0] = kalman.measurementMatrix[1, 1] = 1
    kalman.measurementMatrix[2, 4] = kalman.measurementMatrix[3, 5] = 1
    kalman.processNoiseCov = np.eye(8, dtype=np.float32) * 0.01
    kalman.measurementNoiseCov = np.eye(4, dtype=np.float32)
    kalman.errorCovPost = np.eye(8, dtype=np.float32)
    kalman.statePost = np.array([[x+w/2], [y+h/2], [0], [0], [w], [h], [0], [0]], np.float32)
    weak_frames = stable_frames = 0
    state = "TRACKING"
    frame_id = 0

    try:
        while True:
            ok, frame = camera.read()
            if not ok:
                break
            start = time.perf_counter()
            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            prediction = kalman.predict().reshape(-1)
            pred = np.array([prediction[0]-prediction[4]/2, prediction[1]-prediction[5]/2,
                             max(4, prediction[4]), max(4, prediction[5])], np.float32)

            # LK flow over features inside the last accepted box.
            flow_delta = np.zeros(2, np.float32)
            flow_ratio = 0.0
            mask = np.zeros(prev.shape, np.uint8)
            bx, by, bw, bh = map(int, last_valid)
            bx0, by0 = max(0, bx), max(0, by)
            mask[by0:min(mask.shape[0], by+bh), bx0:min(mask.shape[1], bx+bw)] = 255
            points = cv2.goodFeaturesToTrack(prev, maxCorners=100, qualityLevel=.01, minDistance=4, mask=mask)
            if points is not None and len(points) >= 5:
                nxt, status, errors = cv2.calcOpticalFlowPyrLK(prev, gray, points, None, winSize=(21,21), maxLevel=3)
                good = (status.reshape(-1) == 1) & (errors.reshape(-1) <= 18)
                if np.count_nonzero(good) >= 5:
                    delta = nxt.reshape(-1,2)[good] - points.reshape(-1,2)[good]
                    flow_delta = np.median(delta, axis=0).astype(np.float32)
                    flow_ratio = float(np.count_nonzero(good) / len(points))

            scale = 5.0 if state == "LOST" else (4.0 if state == "WEAK" else 2.5)
            cx, cy = pred[0]+pred[2]/2, pred[1]+pred[3]/2
            sx, sy = max(0, int(cx-pred[2]*scale/2)), max(0, int(cy-pred[3]*scale/2))
            ex, ey = min(gray.shape[1], int(cx+pred[2]*scale/2)), min(gray.shape[0], int(cy+pred[3]*scale/2))
            search = gray[sy:ey, sx:ex]
            best = None
            if search.size:
                for templ in (initial, short):
                    for factor in (.8, 1.0, 1.25):
                        scaled = templ if factor == 1.0 else cv2.resize(templ, None, fx=factor, fy=factor)
                        if search.shape[0] < scaled.shape[0] or search.shape[1] < scaled.shape[1]:
                            continue
                        response = cv2.matchTemplate(search, scaled, cv2.TM_CCOEFF_NORMED)
                        _, corr, _, peak = cv2.minMaxLoc(response)
                        mask_psr = np.ones(response.shape, np.uint8)
                        px0, py0 = max(0, peak[0]-2), max(0, peak[1]-2)
                        mask_psr[py0:py0+5, px0:px0+5] = 0
                        sidelobes = response[mask_psr != 0]
                        psr = float((corr-sidelobes.mean()) / max(float(sidelobes.std()), 1e-5)) if sidelobes.size else 0
                        bw, bh = box[2]*factor, box[3]*factor
                        mx, my = sx+peak[0]+scaled.shape[1]/2, sy+peak[1]+scaled.shape[0]/2
                        candidate = np.array([mx-bw/2, my-bh/2, bw, bh], np.float32)
                        if best is None or corr > best[0]:
                            best = (corr, psr, candidate, (scaled.shape[1], scaled.shape[0]))

            confidence = 0.0
            accepted = False
            if best is not None:
                corr, psr, measured, _ = best
                dx, dy = measured[0]-box[0], measured[1]-box[1]
                flow_agreement = clamp01(flow_ratio/.25) if flow_ratio and np.linalg.norm(np.array([dx,dy])-flow_delta) < max(12, box[2]*.5) else 0.0
                motion_error = np.linalg.norm((measured[:2]+measured[2:]/2)-(pred[:2]+pred[2:]/2))
                motion_agreement = float(np.exp(-motion_error/max(1, np.hypot(pred[2],pred[3])*1.5)))
                corr_norm = clamp01((corr-.38)/.62)
                confidence = .55*corr_norm + .20*clamp01(psr/10) + .15*flow_agreement + .10*motion_agreement
                ratio = max(measured[2]/max(box[2],1), box[2]/max(measured[2],1))
                accepted = corr >= .38 and psr >= 3.0 and confidence >= .43 and ratio <= 1.35
            else:
                corr, psr, measured, flow_agreement = -1, 0, box.copy(), 0

            if accepted:
                z = np.array([[measured[0]+measured[2]/2], [measured[1]+measured[3]/2],
                              [measured[2]], [measured[3]]], np.float32)
                filtered = kalman.correct(z).reshape(-1)
                box = np.array([filtered[0]-filtered[4]/2, filtered[1]-filtered[5]/2,
                                max(4,filtered[4]), max(4,filtered[5])], np.float32)
                last_valid = box.copy()
                if confidence >= .70:
                    state, weak_frames, stable_frames = "TRACKING", 0, stable_frames+1
                else:
                    state, weak_frames, stable_frames = "WEAK", weak_frames+1, 0
                if stable_frames >= 10 and confidence >= .84 and flow_agreement >= .3:
                    patch = gray[max(0,int(box[1])):min(gray.shape[0],int(box[1]+box[3])),
                                 max(0,int(box[0])):min(gray.shape[1],int(box[0]+box[2]))]
                    if patch.size:
                        patch = cv2.resize(patch, (short.shape[1],short.shape[0]))
                        short = cv2.addWeighted(short,.96,patch,.04,0)
            else:
                weak_frames += 1
                if weak_frames <= 3:
                    box = pred
                state = "LOST" if weak_frames >= 5 else "WEAK"
                if state == "LOST":
                    box = last_valid.copy()

            elapsed_ms = (time.perf_counter()-start)*1000
            draw = frame.copy()
            color = (40,220,40) if state == "TRACKING" else ((0,190,255) if state == "WEAK" else (30,30,230))
            rx, ry, rw, rh = map(int, box)
            cv2.rectangle(draw,(rx,ry),(rx+rw,ry+rh),color,2)
            cv2.circle(draw,(rx+rw//2,ry+rh//2),4,color,-1)
            cv2.putText(draw,f"{state}  confidence={confidence:.2f}  latency={elapsed_ms:.1f} ms",
                        (12,28),cv2.FONT_HERSHEY_SIMPLEX,.65,color,2)
            cv2.putText(draw,"Press Q or Esc to close",(12,55),cv2.FONT_HERSHEY_SIMPLEX,.5,(255,255,255),1)
            cv2.imshow("RV1106 tracker — laptop camera preview", draw)
            key = cv2.waitKey(1) & 0xff
            prev = gray
            frame_id += 1
            if key in (ord('q'), 27):
                break
    finally:
        camera.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
