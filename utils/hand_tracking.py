#!/usr/bin/env python3
# Copyright 2026, Tom F.
# SPDX-License-Identifier: BSL-1.0

"""
Monado Phone Driver hand tracking script

This script does hand tracking from phone with MediaPipe holistic landmarker.
This is replacement for the phone's hand tracking and it sends the results in the same format.

Usage:
    python hand_tracking.py
"""

import argparse
import socket
import struct
import time

import cv2
import mediapipe as mp

BaseOptions = mp.tasks.BaseOptions
HolisticLandmarker = mp.tasks.vision.HolisticLandmarker
HolisticLandmarkerOptions = mp.tasks.vision.HolisticLandmarkerOptions
HolisticLandmarkerResult = mp.tasks.vision.HolisticLandmarkerResult
VisionRunningMode = mp.tasks.vision.RunningMode


def flatten_world_relative(world_lm, head):
    out = [0.0] * 63
    for i in range(21):
        out[i * 3 + 0] = float(-(world_lm[i].x - head.x))
        out[i * 3 + 1] = float(-(world_lm[i].y - head.y))
        out[i * 3 + 2] = float(world_lm[i].z - head.z)
    return out


LAST_LEFT = None
LAST_LEFT_TS = 0
LAST_RIGHT = None
LAST_RIGHT_TS = 0
HOLD_NS = 150_000_000

VIZ_RESULT = None

HAND_CONNECTIONS = (
    (0, 1),
    (1, 2),
    (2, 3),
    (3, 4),
    (0, 5),
    (5, 6),
    (6, 7),
    (7, 8),
    (5, 9),
    (9, 10),
    (10, 11),
    (11, 12),
    (9, 13),
    (13, 14),
    (14, 15),
    (15, 16),
    (13, 17),
    (17, 18),
    (18, 19),
    (19, 20),
    (0, 17),
)


def draw_hand(frame, hand_lm, color):
    h, w = frame.shape[:2]
    pts = []
    for lm in hand_lm:
        pts.append((int(lm.x * w), int(lm.y * h)))
    for a, b in HAND_CONNECTIONS:
        cv2.line(frame, pts[a], pts[b], color, 1)
    for p in pts:
        cv2.circle(frame, p, 2, color, -1)


def draw_viz(frame, result):
    if result.pose_landmarks:
        for i, lm in enumerate(result.pose_landmarks):
            x = int(lm.x * frame.shape[1])
            y = int(lm.y * frame.shape[0])
            color = (0, 0, 255) if i == 0 else (200, 200, 200)
            cv2.circle(frame, (x, y), 2 if i else 4, color, -1)
    if result.left_hand_landmarks:
        draw_hand(frame, result.left_hand_landmarks, (0, 255, 0))
    if result.right_hand_landmarks:
        draw_hand(frame, result.right_hand_landmarks, (255, 0, 0))


def send(landmarks):
    data = bytearray()

    data += struct.pack("<q", landmarks["timestamp"])

    flags = 0
    if landmarks["left"] is not None:
        flags |= 1
    if landmarks["right"] is not None:
        flags |= 2

    flags |= 1 << 2  # Do not apply rotation correction on these landmarks

    data += struct.pack("<b", flags)

    if landmarks["left"] is not None:
        data += struct.pack("<63f", *landmarks["left"])

    if landmarks["right"] is not None:
        data += struct.pack("<63f", *landmarks["right"])

    sock.sendto(data, ("127.0.0.1", args.port))


def on_result(
    result: HolisticLandmarkerResult, output_image: mp.Image, timestamp_ms: int
):
    global LAST_LEFT, LAST_LEFT_TS, LAST_RIGHT, LAST_RIGHT_TS, VIZ_RESULT
    VIZ_RESULT = result
    now_ns = timestamp_ms * 1_000_000
    landmarks = {
        "timestamp": now_ns,
        "left": None,
        "right": None,
    }

    head = result.pose_world_landmarks[0] if result.pose_world_landmarks else None

    if head is not None:
        if result.left_hand_world_landmarks:
            landmarks["left"] = flatten_world_relative(
                result.left_hand_world_landmarks, head
            )
            LAST_LEFT = landmarks["left"]
            LAST_LEFT_TS = now_ns
        if result.right_hand_world_landmarks:
            landmarks["right"] = flatten_world_relative(
                result.right_hand_world_landmarks, head
            )
            LAST_RIGHT = landmarks["right"]
            LAST_RIGHT_TS = now_ns

    if (
        landmarks["left"] is None
        and LAST_LEFT is not None
        and now_ns - LAST_LEFT_TS < HOLD_NS
    ):
        landmarks["left"] = LAST_LEFT
    if (
        landmarks["right"] is None
        and LAST_RIGHT is not None
        and now_ns - LAST_RIGHT_TS < HOLD_NS
    ):
        landmarks["right"] = LAST_RIGHT

    send(landmarks)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Monado Phone Driver hand tracking script from PC camera, sends the results in the same format as app."
    )
    parser.add_argument(
        "-p",
        "--port",
        type=int,
        default=5504,
        help="UDP port to send the data to (default: 5504)",
    )
    parser.add_argument(
        "-v",
        "--video",
        type=int,
        default=0,
        help="Video device to use (default: 0)",
    )
    parser.add_argument(
        "--no-viz",
        action="store_true",
        help="Disable OpenCV visualization window",
    )

    args = parser.parse_args()

    cap = cv2.VideoCapture(args.video)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    options = HolisticLandmarkerOptions(
        base_options=BaseOptions(
            model_asset_path="holistic_landmarker.task",
            delegate=BaseOptions.Delegate.CPU,  # GPU throws error STRIDED_SLICE: Slice does not support shrink_axis_mask parameter.
        ),
        running_mode=VisionRunningMode.LIVE_STREAM,
        min_face_detection_confidence=0.5,
        min_pose_detection_confidence=0.5,
        min_hand_landmarks_confidence=0.5,
        result_callback=on_result,
    )

    with HolisticLandmarker.create_from_options(options) as landmarker:
        while True:
            ret, frame = cap.read()
            if not ret:
                break

            rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)

            mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
            timestamp_ms = time.monotonic_ns() // 1_000_000

            result = landmarker.detect_async(mp_image, timestamp_ms)

            if not args.no_viz:
                if VIZ_RESULT is not None:
                    draw_viz(frame, VIZ_RESULT)
                cv2.imshow("hand_tracking", frame)

                key = cv2.waitKey(1) & 0xFF
                if key == ord("q") or key == 27:
                    break

    cap.release()
    cv2.destroyAllWindows()
