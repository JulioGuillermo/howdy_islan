"""Sends Howdy's scan, or howdy add's enrolment, to the howdy-island system service.

Called from the hooks that system/patch_hooks.py adds to Howdy's compare.py and cli/add.py. Never
raises: without the service listening, every call is a no-op and Howdy carries on as usual.

One datagram per message, a letter then its data: S uid user, R timeout threshold, G gray frame
(width and height as little-endian 16-bit numbers, then the pixels), D face boxes (4 fractions
each), C certainty, E exit code. Enrolment: N start, P progress fraction, A result. The Gaze feed
(gaze/feed) also sends H, a hint line, and X, cancelled.
"""
import os
import pwd
import socket
import struct

import cv2

SOCKET_PATH = "/run/howdy-island/island.sock"
PREVIEW_WIDTH = 192

_sender = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
_sender.setblocking(False)
_socket_paths = []


def begin(user):
	uid = _uid_of(user)
	if uid is None or not _is_owned_by_root(SOCKET_PATH):
		return
	_socket_paths.append(SOCKET_PATH)
	_send(b"S", f"{uid} {user}".encode())


def begin_enrolment(user):
	begin(user)
	_send(b"N", b"")


def enrolment_progress(fraction):
	_send(b"P", f"{fraction:.3f}".encode())


def enrolment_finished(reason):
	_send(b"A", reason.encode())


def scan(timeout, threshold):
	_send(b"R", f"{timeout} {threshold}".encode())


def frame(gray_frame):
	if not _socket_paths:
		return
	height, width = gray_frame.shape[:2]
	preview_height = round(height * PREVIEW_WIDTH / width)
	preview = cv2.resize(gray_frame, (PREVIEW_WIDTH, preview_height), interpolation=cv2.INTER_AREA)
	_send(b"G", struct.pack("<HH", PREVIEW_WIDTH, preview_height) + preview.tobytes())


def faces(face_locations, frame_shape, use_cnn):
	height, width = frame_shape[:2]
	values = []
	for location in face_locations:
		rect = location.rect if use_cnn else location
		values += [rect.left() / width, rect.top() / height, rect.width() / width, rect.height() / height]
	_send(b"D", " ".join(f"{value:.4f}" for value in values).encode())


def certainty(value):
	_send(b"C", f"{float(value):.3f}".encode())


def finish(exit_code):
	if exit_code is not None:
		_send(b"E", str(exit_code).encode())


def _send(kind, payload):
	for path in _socket_paths:
		try:
			_sender.sendto(kind + payload, path)
		except OSError:
			pass


def _uid_of(user):
	try:
		return pwd.getpwnam(user).pw_uid
	except KeyError:
		return None


def _is_owned_by_root(path):
	try:
		return os.lstat(path).st_uid == 0
	except OSError:
		return False
