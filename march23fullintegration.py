
import os
import cv2
import time
import math
import atexit
import signal
import serial
import numpy as np
import onnxruntime as ort
import socket
import threading

# =========================
# OPTIONAL: Picamera2
# =========================
USE_PICAMERA2 = True
SOURCE = 0  # used only if USE_PICAMERA2 = False

try:
    from picamera2 import Picamera2
except Exception:
    Picamera2 = None
    if USE_PICAMERA2:
        print("[WARN] Picamera2 not available, falling back to cv2.VideoCapture")
        USE_PICAMERA2 = False

# =========================
# OPTIONAL: Raspberry Pi GPIO
# =========================
try:
    import RPi.GPIO as GPIO
    GPIO_AVAILABLE = True
except Exception:
    GPIO = None
    GPIO_AVAILABLE = False
    print("[WARN] RPi.GPIO not available; ultrasonic obstacle avoidance disabled")

# =========================
# USER SETTINGS
# =========================
MODEL_PATH = "yolov8n.onnx"
OUTPUT_DIR = "runs_onnx"
CONF_THRES = 0.35
IOU_THRES = 0.45
INPUT_SIZE = 640
CAM_W = 640
CAM_H = 480
HALF_FOV_RAD = 0.55
PERSON_H_M = 1.70
FOCAL_LEN_PX = 700.0

# Track tuning
MAX_TRACK_MISSES = 12
MAX_MATCH_DIST = 120.0

# Save video
SAVE_VIDEO = False

# =========================
# SERIAL (ESP32)
# =========================
SERIAL_PORT = "/dev/ttyACM0"
BAUD = 115200
PWM_MAX = 1023

# Motor tuning
BASE_PWM = 400
FAST_PWM = 800
TURN_PWM = 500
MIN_MOVE_PWM = 300
STOP_DIST_M = 1.5
FAR_DIST_M = 3.5
DESIRED_DIST_M = 2.0
NO_TARGET_STOP = True

ser = None

# =========================
# MANUAL OVERRIDE
# =========================
manual_command = "S"
manual_timeout = 0.0
manual_mode_enabled = False
manual_lock = threading.Lock()
MANUAL_HOLD_S = 1.0
UDP_PORT = 5006

# =========================
# ULTRASONIC SETTINGS
# Shared TRIG + 4 ECHOs
# IMPORTANT:
# - HC-SR04 echo pins must be level shifted / voltage divided to 3.3V for Pi GPIO.
# - With shared TRIG we pulse once per sensor read to reduce cross-talk.
# =========================
USE_ULTRASONICS = True

US_TRIG_PIN = 24
US_ECHO_PINS = {
    "front": 17,
    "left": 27,
    "right": 23,
    "back": 22,
}

US_MIN_M = 0.02
US_MAX_M = 4.00
US_INTER_SENSOR_DELAY_S = 0.03
US_LOOP_DELAY_S = 0.02
US_TIMEOUT_S = 0.025
US_FILTER_ALPHA = 0.45

US_STOP_FRONT_M = 0.85
US_CAUTION_FRONT_M = 1.1
US_STOP_BACK_M = 0.5
US_SIDE_CLEAR_M = 0.45
US_AVOID_TURN_PWM = 500
US_AVOID_FWD_PWM = 500
US_AVOID_FAST_FWD_PWM = 900

ultra = {name: float("nan") for name in US_ECHO_PINS}
ultra_lock = threading.Lock()
ultra_last_update_s = 0.0
ultra_thread_started = False

# =========================
# COCO CLASS NAMES
# =========================
CLASS_NAMES = [
    "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat",
    "traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse",
    "sheep","cow","elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie",
    "suitcase","frisbee","skis","snowboard","sports ball","kite","baseball bat","baseball glove",
    "skateboard","surfboard","tennis racket","bottle","wine glass","cup","fork","knife","spoon",
    "bowl","banana","apple","sandwich","orange","broccoli","carrot","hot dog","pizza","donut",
    "cake","chair","couch","potted plant","bed","dining table","toilet","tv","laptop","mouse",
    "remote","keyboard","cell phone","microwave","oven","toaster","sink","refrigerator","book",
    "clock","vase","scissors","teddy bear","hair drier","toothbrush"
]

# =========================
# HELPERS
# =========================
def clamp(x, lo, hi):
    return max(lo, min(hi, x))

def iou_xyxy(a, b):
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b

    inter_x1 = max(ax1, bx1)
    inter_y1 = max(ay1, by1)
    inter_x2 = min(ax2, bx2)
    inter_y2 = min(ay2, by2)

    iw = max(0.0, inter_x2 - inter_x1)
    ih = max(0.0, inter_y2 - inter_y1)
    inter = iw * ih

    area_a = max(0.0, ax2 - ax1) * max(0.0, ay2 - ay1)
    area_b = max(0.0, bx2 - bx1) * max(0.0, by2 - by1)
    union = area_a + area_b - inter + 1e-9

    return inter / union

def nms_xyxy(dets, iou_thres=0.45):
    if not dets:
        return []

    dets = sorted(dets, key=lambda d: d["score"], reverse=True)
    keep = []

    while dets:
        best = dets.pop(0)
        keep.append(best)
        remaining = []
        for d in dets:
            if d["cls_id"] != best["cls_id"] or iou_xyxy(d["box"], best["box"]) < iou_thres:
                remaining.append(d)
        dets = remaining

    return keep

def make_output_path(output_dir):
    os.makedirs(output_dir, exist_ok=True)
    i = 1
    while os.path.exists(os.path.join(output_dir, f"{i}.mp4")):
        i += 1
    return os.path.join(output_dir, f"{i}.mp4")

def estimate_distance_m(box, label):
    x1, y1, x2, y2 = box
    h = max(1.0, y2 - y1)
    w = max(1.0, x2 - x1)

    if label == "person":
        return (PERSON_H_M * FOCAL_LEN_PX) / h

    area = max(1.0, w * h)
    return 2500.0 / math.sqrt(area)

# =========================
# SERIAL HELPERS
# =========================
def init_serial():
    global ser
    print(f"[INFO] Opening serial: {SERIAL_PORT}")
    ser = serial.Serial(
        SERIAL_PORT,
        BAUD,
        timeout=1,
        write_timeout=1,
        rtscts=False,
        dsrdtr=False,
        xonxoff=False
    )
    try:
        ser.setDTR(False)
        ser.setRTS(False)
    except Exception:
        pass
    time.sleep(2)
    print("[INFO] Serial connected")

def send_motor_pwm(m1, m2):
    global ser
    if ser is None:
        return

    m1 = int(clamp(m1, -PWM_MAX, PWM_MAX))
    m2 = int(clamp(m2, -PWM_MAX, PWM_MAX))

    msg = f"{m1},{m2}\n"
    print(f"[SERIAL OUT] {msg.strip()}")
    ser.write(msg.encode("utf-8"))
    ser.flush()

def stop_motors():
    try:
        send_motor_pwm(0, 0)
    except Exception:
        pass

def close_serial():
    global ser
    try:
        stop_motors()
    except Exception:
        pass
    if ser is not None:
        try:
            ser.close()
        except Exception:
            pass
        ser = None

# =========================
# ULTRASONIC HELPERS
# =========================
def init_ultrasonics():
    global ultra_thread_started
    if not USE_ULTRASONICS:
        print("[INFO] Ultrasonics disabled in settings")
        return
    if not GPIO_AVAILABLE:
        print("[WARN] Ultrasonics requested but GPIO not available")
        return
    if ultra_thread_started:
        return

    GPIO.setmode(GPIO.BCM)
    GPIO.setup(US_TRIG_PIN, GPIO.OUT)
    GPIO.output(US_TRIG_PIN, False)

    for echo_pin in US_ECHO_PINS.values():
        GPIO.setup(echo_pin, GPIO.IN)

    time.sleep(0.25)
    ultra_thread_started = True
    threading.Thread(target=ultrasonic_thread, daemon=True).start()
    print(f"[INFO] Ultrasonic thread started: TRIG={US_TRIG_PIN}, ECHOs={US_ECHO_PINS}")

def close_ultrasonics():
    if GPIO_AVAILABLE and ultra_thread_started:
        try:
            GPIO.cleanup()
        except Exception:
            pass

def trigger_pulse():
    GPIO.output(US_TRIG_PIN, True)
    time.sleep(0.00001)
    GPIO.output(US_TRIG_PIN, False)

def read_echo_distance_m(echo_pin):
    start_wait = time.time()
    deadline = start_wait + US_TIMEOUT_S

    while GPIO.input(echo_pin) == 0:
        if time.time() > deadline:
            return None

    pulse_start = time.time()
    deadline = pulse_start + US_TIMEOUT_S

    while GPIO.input(echo_pin) == 1:
        if time.time() > deadline:
            return None

    pulse_end = time.time()
    elapsed = pulse_end - pulse_start
    dist_m = (elapsed * 343.0) / 2.0

    if not (US_MIN_M <= dist_m <= US_MAX_M):
        return None
    return dist_m

def low_pass_filter(new_value, old_value, alpha=US_FILTER_ALPHA):
    if old_value is None or not np.isfinite(old_value):
        return new_value
    return (alpha * new_value) + ((1.0 - alpha) * old_value)

def ultrasonic_thread():
    global ultra_last_update_s

    while True:
        cycle_values = {}

        for name, echo_pin in US_ECHO_PINS.items():
            try:
                trigger_pulse()
                dist_m = read_echo_distance_m(echo_pin)
                if dist_m is not None:
                    cycle_values[name] = dist_m
            except Exception as e:
                print(f"[US] Read error on {name}: {e}")

            time.sleep(US_INTER_SENSOR_DELAY_S)

        if cycle_values:
            with ultra_lock:
                for name, dist_m in cycle_values.items():
                    ultra[name] = low_pass_filter(dist_m, ultra.get(name, float("nan")))
                ultra_last_update_s = time.time()

        time.sleep(US_LOOP_DELAY_S)

def get_ultra_snapshot():
    with ultra_lock:
        snap = dict(ultra)
        last_update = ultra_last_update_s
    return snap, last_update

def obstacle_override():
    snap, _ = get_ultra_snapshot()
    f = snap.get("front", float("nan"))
    l = snap.get("left", float("nan"))
    r = snap.get("right", float("nan"))
    b = snap.get("back", float("nan"))

    # Highest priority: front obstacle avoidance.
    if np.isfinite(f) and f < US_STOP_FRONT_M:
        left_clear = np.isfinite(l) and l > US_SIDE_CLEAR_M
        right_clear = np.isfinite(r) and r > US_SIDE_CLEAR_M

        if left_clear and (not right_clear or l >= r):
            return -US_AVOID_TURN_PWM, US_AVOID_TURN_PWM, "AVOID_LEFT"
        if right_clear:
            return US_AVOID_TURN_PWM, -US_AVOID_TURN_PWM, "AVOID_RIGHT"
        return 0, 0, "BLOCKED_FRONT"

    # Front caution band: keep moving slowly but bias away from closer side.
    if np.isfinite(f) and f < US_CAUTION_FRONT_M:
        if np.isfinite(l) and np.isfinite(r):
            if l >= r:
                return US_AVOID_FWD_PWM, US_AVOID_FAST_FWD_PWM, "BIAS_LEFT"
            return US_AVOID_FAST_FWD_PWM, US_AVOID_FWD_PWM, "BIAS_RIGHT"
        if np.isfinite(l):
            return US_AVOID_FWD_PWM, US_AVOID_FAST_FWD_PWM, "BIAS_LEFT"
        if np.isfinite(r):
            return US_AVOID_FAST_FWD_PWM, US_AVOID_FWD_PWM, "BIAS_RIGHT"

    # Back sensor is enforced when a reverse command is requested.
    return None

def apply_reverse_safety(m1, m2):
    snap, _ = get_ultra_snapshot()
    back_m = snap.get("back", float("nan"))

    if m1 < 0 and m2 < 0 and np.isfinite(back_m) and back_m < US_STOP_BACK_M:
        return 0, 0, "BACK_BLOCKED"

    return m1, m2, None

# =========================
# SIMPLE CENTROID TRACKER
# =========================
class SimpleTracker:
    def __init__(self, max_misses=10, max_match_dist=100.0):
        self.max_misses = max_misses
        self.max_match_dist = max_match_dist
        self.next_id = 1
        self.tracks = {}

    def _center(self, box):
        x1, y1, x2, y2 = box
        return np.array([(x1 + x2) * 0.5, (y1 + y2) * 0.5], dtype=np.float32)

    def update(self, detections):
        assigned_track_ids = set()
        assigned_det_idx = set()

        track_ids = list(self.tracks.keys())
        for tid in track_ids:
            tr = self.tracks[tid]
            best_idx = -1
            best_dist = float("inf")

            tr_center = self._center(tr["box"])
            tr_cls = tr["cls_id"]

            for i, det in enumerate(detections):
                if i in assigned_det_idx:
                    continue
                if det["cls_id"] != tr_cls:
                    continue

                det_center = self._center(det["box"])
                dist = float(np.linalg.norm(det_center - tr_center))
                if dist < best_dist:
                    best_dist = dist
                    best_idx = i

            if best_idx >= 0 and best_dist <= self.max_match_dist:
                det = detections[best_idx]
                self.tracks[tid] = {
                    "box": det["box"],
                    "cls_id": det["cls_id"],
                    "label": det["label"],
                    "score": det["score"],
                    "misses": 0,
                }
                detections[best_idx]["track_id"] = tid
                assigned_track_ids.add(tid)
                assigned_det_idx.add(best_idx)

        for i, det in enumerate(detections):
            if i in assigned_det_idx:
                continue
            tid = self.next_id
            self.next_id += 1
            self.tracks[tid] = {
                "box": det["box"],
                "cls_id": det["cls_id"],
                "label": det["label"],
                "score": det["score"],
                "misses": 0,
            }
            det["track_id"] = tid
            assigned_track_ids.add(tid)

        dead = []
        for tid, tr in self.tracks.items():
            if tid not in assigned_track_ids:
                tr["misses"] += 1
                if tr["misses"] > self.max_misses:
                    dead.append(tid)

        for tid in dead:
            del self.tracks[tid]

        return detections

# =========================
# YOLOv8 ONNX
# =========================
class YOLOv8ONNX:
    def __init__(self, model_path):
        providers = ["CPUExecutionProvider"]
        self.session = ort.InferenceSession(model_path, providers=providers)
        self.input_name = self.session.get_inputs()[0].name
        self.output_names = [o.name for o in self.session.get_outputs()]
        input_shape = self.session.get_inputs()[0].shape

        if len(input_shape) >= 4 and isinstance(input_shape[2], int) and isinstance(input_shape[3], int):
            self.input_h = input_shape[2]
            self.input_w = input_shape[3]
        else:
            self.input_h = INPUT_SIZE
            self.input_w = INPUT_SIZE

        print(f"[INFO] ONNX input: {self.input_w}x{self.input_h}")
        print(f"[INFO] ONNX outputs: {self.output_names}")

    def letterbox(self, image, new_shape=(640, 640), color=(114, 114, 114)):
        h, w = image.shape[:2]
        new_w, new_h = new_shape
        r = min(new_w / w, new_h / h)

        resized_w = int(round(w * r))
        resized_h = int(round(h * r))

        resized = cv2.resize(image, (resized_w, resized_h), interpolation=cv2.INTER_LINEAR)

        dw = new_w - resized_w
        dh = new_h - resized_h
        dw /= 2
        dh /= 2

        top = int(round(dh - 0.1))
        bottom = int(round(dh + 0.1))
        left = int(round(dw - 0.1))
        right = int(round(dw + 0.1))

        padded = cv2.copyMakeBorder(resized, top, bottom, left, right, cv2.BORDER_CONSTANT, value=color)
        return padded, r, left, top

    def preprocess(self, image):
        if not USE_PICAMERA2:
            image = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)

        blob, ratio, pad_x, pad_y = self.letterbox(image, (self.input_w, self.input_h))
        blob = blob.astype(np.float32) / 255.0
        blob = np.transpose(blob, (2, 0, 1))
        blob = np.expand_dims(blob, axis=0)
        return blob, ratio, pad_x, pad_y

    def infer(self, image):
        blob, ratio, pad_x, pad_y = self.preprocess(image)
        outputs = self.session.run(self.output_names, {self.input_name: blob})
        return outputs, ratio, pad_x, pad_y

    def postprocess(self, outputs, orig_w, orig_h, ratio, pad_x, pad_y,
                    conf_thres=0.35, iou_thres=0.45):
        pred = outputs[0]

        if pred.ndim == 3:
            pred = pred[0]

        if pred.ndim != 2:
            raise RuntimeError(f"Unexpected ONNX output shape: {outputs[0].shape}")

        if pred.shape[0] < pred.shape[1] and pred.shape[0] in (84, 85):
            pred = pred.T

        detections = []
        num_classes = len(CLASS_NAMES)

        for row in pred:
            if len(row) < 4 + num_classes:
                continue

            x, y, w, h = row[:4]
            class_scores = row[4:4 + num_classes]
            cls_id = int(np.argmax(class_scores))
            score = float(class_scores[cls_id])

            if score < conf_thres:
                continue

            x1 = x - w / 2.0
            y1 = y - h / 2.0
            x2 = x + w / 2.0
            y2 = y + h / 2.0

            x1 = (x1 - pad_x) / ratio
            y1 = (y1 - pad_y) / ratio
            x2 = (x2 - pad_x) / ratio
            y2 = (y2 - pad_y) / ratio

            x1 = clamp(x1, 0, orig_w - 1)
            y1 = clamp(y1, 0, orig_h - 1)
            x2 = clamp(x2, 0, orig_w - 1)
            y2 = clamp(y2, 0, orig_h - 1)

            if x2 <= x1 or y2 <= y1:
                continue

            label = CLASS_NAMES[cls_id] if cls_id < len(CLASS_NAMES) else f"class_{cls_id}"
            detections.append({
                "box": [float(x1), float(y1), float(x2), float(y2)],
                "score": score,
                "cls_id": cls_id,
                "label": label,
            })

        return nms_xyxy(detections, iou_thres=iou_thres)

# =========================
# CAMERA SETUP
# =========================
def make_camera():
    if USE_PICAMERA2 and Picamera2 is not None:
        picam2 = Picamera2()
        config = picam2.create_preview_configuration(
            main={"size": (CAM_W, CAM_H), "format": "RGB888"}
        )
        picam2.configure(config)
        picam2.start()
        time.sleep(1.0)
        return picam2, None

    cap = cv2.VideoCapture(SOURCE)
    if not cap.isOpened():
        raise RuntimeError("Could not open camera/video")
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, CAM_W)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, CAM_H)
    return None, cap

def read_frame(picam2, cap):
    if picam2 is not None:
        return picam2.capture_array()
    ok, frame = cap.read()
    if not ok:
        return None
    return frame

# =========================
# TARGET + CONTROL
# =========================
def pick_target(tracked):
    best = None
    best_dist = float("inf")

    for det in tracked:
        if det["label"] != "person":
            continue
        d = estimate_distance_m(det["box"], "person")
        if d < best_dist:
            best_dist = d
            best = det

    return best, best_dist

def compute_motor_command(target, target_dist_m, frame_w):
    if target is None:
        return 0, 0, "LOST"

    x1, y1, x2, y2 = target["box"]
    cx = 0.5 * (x1 + x2)
    error = (cx - (frame_w * 0.5)) / (frame_w * 0.5)

    # 1) Hard safety stop: way too close
    if target_dist_m <= STOP_DIST_M:
        return 0, 0, "STOP"

    # 2) Hold position once inside desired follow distance
    dist_error = target_dist_m - DESIRED_DIST_M
    if dist_error <= 0.0:
        return 0, 0, "HOLD"

    # 3) Proportional forward speed based on how far beyond desired distance we are
    span = max(0.01, FAR_DIST_M - DESIRED_DIST_M)
    alpha = clamp(dist_error / span, 0.0, 1.0)
    base = int(MIN_MOVE_PWM + alpha * (FAST_PWM - MIN_MOVE_PWM))

    # Steering correction
    turn = int(TURN_PWM * error)

    m1 = base + turn
    m2 = base - turn

    # Only enforce minimum move power when actually trying to move forward
    if dist_error > 0.15:
        if m1 > 0:
            m1 = max(MIN_MOVE_PWM, m1)
        if m2 > 0:
            m2 = max(MIN_MOVE_PWM, m2)

    m1 = int(clamp(m1, -PWM_MAX, PWM_MAX))
    m2 = int(clamp(m2, -PWM_MAX, PWM_MAX))

    return m1, m2, "FOLLOW"



def manual_command_to_pwm(cmd):
    if cmd == "F":
        return FAST_PWM, FAST_PWM, "MANUAL-F"
    if cmd == "B":
        return -FAST_PWM, -FAST_PWM, "MANUAL-B"
    if cmd == "L":
        return -TURN_PWM, TURN_PWM, "MANUAL-L"
    if cmd == "R":
        return TURN_PWM, -TURN_PWM, "MANUAL-R"
    return 0, 0, "MANUAL-S"

def set_manual_mode(enabled, stop_now=False):
    global manual_mode_enabled, manual_command, manual_timeout
    with manual_lock:
        manual_mode_enabled = enabled
        manual_timeout = 0.0
        if stop_now:
            manual_command = "S"

def get_manual_state():
    with manual_lock:
        return manual_mode_enabled, manual_command, manual_timeout

# =========================
# UDP MOTOR LI
#STENER
# =========================
def udp_motor_listener():
    global manual_command, manual_timeout, manual_mode_enabled

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", UDP_PORT))
    sock.settimeout(0.5)

    print(f"[UDP] Listening on port {UDP_PORT}")
    print("[UDP] Manual mode commands: MANUAL / AUTO")
    print("[UDP] Drive commands: F B L R S")
    print("[UDP] Note: F/B/L/R/S force manual mode ON until AUTO is received.")

    while True:
        try:
            data, addr = sock.recvfrom(1024)
            cmd = data.decode("utf-8").strip().upper()

            if cmd in ["MANUAL", "M", "MAN", "JOY"]:
                set_manual_mode(True, stop_now=True)
                print(f"[UDP] From {addr}: MANUAL mode enabled")
                continue

            if cmd in ["AUTO", "A", "TRACK", "YOLO"]:
                set_manual_mode(False, stop_now=True)
                stop_motors()
                print(f"[UDP] From {addr}: AUTO mode enabled")
                continue

            if cmd in ["F", "B", "L", "R", "S"]:
                with manual_lock:
                    manual_mode_enabled = True
                    manual_command = cmd
                    manual_timeout = time.time() + MANUAL_HOLD_S
                print(f"[UDP] From {addr}: {cmd} (manual mode latched)")
                continue

            print(f"[UDP] Ignored unknown command from {addr}: {cmd}")

        except socket.timeout:
            pass
        except Exception as e:
            print(f"[UDP] Error: {e}")

# =========================
# MAIN
# =========================
def main():
    print("[INFO] Starting yolo_avoid_serial.py")
    print(f"[INFO] Model: {MODEL_PATH}")

    udp_thread = threading.Thread(target=udp_motor_listener, daemon=True)
    udp_thread.start()

    detector = YOLOv8ONNX(MODEL_PATH)
    tracker = SimpleTracker(
        max_misses=MAX_TRACK_MISSES,
        max_match_dist=MAX_MATCH_DIST
    )

    init_serial()
    init_ultrasonics()
    atexit.register(close_serial)
    atexit.register(close_ultrasonics)

    def handle_exit(sig, frame):
        close_serial()
        close_ultrasonics()
        raise KeyboardInterrupt

    signal.signal(signal.SIGINT, handle_exit)
    signal.signal(signal.SIGTERM, handle_exit)

    picam2, cap = make_camera()

    out = None
    out_path = None
    if SAVE_VIDEO:
        out_path = make_output_path(OUTPUT_DIR)
        fourcc = cv2.VideoWriter_fourcc(*"mp4v")
        out = cv2.VideoWriter(out_path, fourcc, 20.0, (CAM_W, CAM_H))
        print(f"[INFO] Saving video to: {out_path}")

    fps_t0 = time.time()
    fps_count = 0
    fps_val = 0.0

    last_mode = "ACQUIRE"
    last_target_id = -1
    last_m1 = 0
    last_m2 = 0
    last_dist = 0.0

    try:
        while True:
            frame = read_frame(picam2, cap)
            if frame is None:
                print("[WARN] No frame received")
                break

            H, W = frame.shape[:2]

            outputs, ratio, pad_x, pad_y = detector.infer(frame)
            detections = detector.postprocess(
                outputs, W, H, ratio, pad_x, pad_y,
                conf_thres=CONF_THRES,
                iou_thres=IOU_THRES
            )

            tracked = tracker.update(detections)

            person_count = 0
            object_count = 0
            vis = frame.copy()

            for det in tracked:
                x1, y1, x2, y2 = map(int, det["box"])
                label = det["label"]
                score = det["score"]
                track_id = det.get("track_id", -1)

                if label == "person":
                    person_count += 1
                    color = (0, 255, 0)
                    thickness = 3
                else:
                    object_count += 1
                    color = (255, 128, 0)
                    thickness = 2

                label_text = f"{label} ID={track_id} {score:.2f}"
                cv2.rectangle(vis, (x1, y1), (x2, y2), color, thickness)

                (tw, th), _ = cv2.getTextSize(label_text, cv2.FONT_HERSHEY_SIMPLEX, 0.55, 2)
                top_y = max(0, y1 - th - 8)
                cv2.rectangle(vis, (x1, top_y), (x1 + tw + 4, y1), color, -1)
                cv2.putText(
                    vis, label_text, (x1 + 2, max(th + 2, y1 - 4)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 0, 0), 2, cv2.LINE_AA
                )

                cx = int((x1 + x2) / 2)
                cy = int((y1 + y2) / 2)
                cv2.circle(vis, (cx, cy), 3, color, -1)

            target, target_dist_m = pick_target(tracked)

            # Control priority:
            # 1) Ultrasonic obstacle avoidance
            # 2) Manual UDP command
            # 3) YOLO person follow
            # 4) Stop if no target
            override = obstacle_override()

            manual_enabled, cmd, timeout_until = get_manual_state()

            if override is not None:
                m1, m2, mode_name = override
                target_id = -1
                target_dist_m = 0.0
            elif manual_enabled:
                # In manual mode, YOLO must never command the motors.
                # The rover either executes the last manual command during its hold window
                # or sits still waiting for the next manual command.
                if time.time() < timeout_until:
                    m1, m2, mode_name = manual_command_to_pwm(cmd)
                else:
                    m1, m2, mode_name = 0, 0, "MANUAL-IDLE"
                m1, m2, safety_mode = apply_reverse_safety(m1, m2)
                if safety_mode is not None:
                    mode_name = safety_mode
                target_id = -1
                target_dist_m = 0.0
            else:
                if target is None and NO_TARGET_STOP:
                    m1, m2, mode_name = 0, 0, "LOST"
                    target_id = -1
                    target_dist_m = 0.0
                else:
                    m1, m2, mode_name = compute_motor_command(target, target_dist_m, W)
                    m1, m2, safety_mode = apply_reverse_safety(m1, m2)
                    if safety_mode is not None:
                        mode_name = safety_mode
                    target_id = target.get("track_id", -1) if target is not None else -1

            send_motor_pwm(m2, m1)

            last_mode = mode_name
            last_target_id = target_id
            last_m1 = m1
            last_m2 = m2
            last_dist = target_dist_m

            if target is not None:
                x1, y1, x2, y2 = map(int, target["box"])
                cv2.rectangle(vis, (x1, y1), (x2, y2), (255, 0, 255), 3)
                cv2.putText(
                    vis, "TARGET", (x1, max(20, y1 - 10)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 0, 255), 2, cv2.LINE_AA
                )

            fps_count += 1
            now = time.time()
            if now - fps_t0 >= 1.0:
                fps_val = fps_count / (now - fps_t0)
                fps_t0 = now
                fps_count = 0

            ultra_snap, ultra_ts = get_ultra_snapshot()
            manual_enabled, cmd_now, timeout_now = get_manual_state()
            overlay_lines = [
                f"FPS: {fps_val:.1f}",
                f"People: {person_count} | Objects: {object_count}",
                f"Mode: {last_mode}",
                f"Manual: {'ON' if manual_enabled else 'OFF'} Cmd:{cmd_now} Hold:{max(0.0, timeout_now - time.time()):.2f}s",
                f"Target ID: {last_target_id}",
                f"Target Dist m: {last_dist:.2f}",
                f"M1: {last_m1}  M2: {last_m2}",
                f"US F:{ultra_snap['front']:.2f} L:{ultra_snap['left']:.2f} R:{ultra_snap['right']:.2f} B:{ultra_snap['back']:.2f}",
                f"US age: {max(0.0, time.time() - ultra_ts):.2f}s",
            ]

            y = 25
            for line_text in overlay_lines:
                cv2.putText(
                    vis, line_text, (10, y),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.65, (0, 255, 255), 2, cv2.LINE_AA
                )
                y += 28

            ##cv2.imshow("live_onnx serial motor control", vis)

            if out is not None:
                out.write(cv2.cvtColor(vis, cv2.COLOR_RGB2BGR) if USE_PICAMERA2 else vis)

 #           key = cv2.waitKey(1) & 0xFF
  #          if key == ord("q"):
   #             break

    finally:
        stop_motors()

        if picam2 is not None:
            try:
                picam2.stop()
            except Exception:
                pass
        if cap is not None:
            cap.release()
        if out is not None:
            out.release()

        close_serial()
        close_ultrasonics()
        cv2.destroyAllWindows()

        if out_path is not None:
            print("Saved:", out_path)

if __name__ == "__main__":
    main()



# =========================
# UWB (FUTURE INTEGRATION - CURRENTLY DISABLED)
# =========================
# This section is pre-wired for UWB AoA tracking (Makerfabs kit)
# It is fully commented out so it will NOT affect current behavior.

'''
# ===== UWB STATE =====
uwb_data = {
    "distance_m": None,
    "angle_deg": None,
    "valid": False,
    "last_update": 0.0
}
uwb_lock = threading.Lock()

# ===== UWB THREAD =====
def uwb_thread():
    import serial
    global uwb_data

    ser = serial.Serial("/dev/ttyUSB0", 115200, timeout=0.1)

    while True:
        line = ser.readline().decode(errors="ignore").strip()
        if not line:
            continue

        try:
            if "DIST" in line and "ANGLE" in line:
                parts = line.replace(":", " ").split()

                dist = float(parts[parts.index("DIST") + 1])
                angle = float(parts[parts.index("ANGLE") + 1])

                with uwb_lock:
                    uwb_data["distance_m"] = dist
                    uwb_data["angle_deg"] = angle
                    uwb_data["valid"] = True
                    uwb_data["last_update"] = time.time()
        except Exception:
            pass

# threading.Thread(target=uwb_thread, daemon=True).start()

# ===== UWB CONTROL (fallback tracking) =====
def uwb_follow_command():
    with uwb_lock:
        if not uwb_data["valid"]:
            return None

        dist = uwb_data["distance_m"]
        angle = uwb_data["angle_deg"]

    if time.time() - uwb_data["last_update"] > 0.5:
        return None

    KP_TURN = 0.02
    turn = KP_TURN * angle

    if dist > 1.5:
        forward = 250
    elif dist > 1.0:
        forward = 150
    else:
        forward = 0

    m1 = int(forward - turn * 300)
    m2 = int(forward + turn * 300)

    return m1, m2, "UWB_TRACK"

# ===== HOW TO ENABLE LATER =====
# 1. Uncomment uwb_thread() start
# 2. Add fallback in main loop AFTER YOLO:
#
# else:
#     uwb_cmd = uwb_follow_command()
#     if uwb_cmd is not None:
#         m1, m2, mode = uwb_cmd
#     else:
#         m1, m2, mode = 0, 0, "NO_TARGET"
'''
