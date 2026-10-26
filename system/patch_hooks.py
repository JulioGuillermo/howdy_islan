"""Adds the howdy-island lines to another program, or removes them with --remove.

  patch_hooks.py howdy      Howdy's compare.py (authentication) and cli/add.py (howdy add) send
                            their scan to the island
  patch_hooks.py nwg-hello  the nwg-hello greeter answers PAM's info and error messages, which
                            Gaze sends while it scans, instead of hanging on them

Run again after every upgrade of that program (the pacman hooks do it). Each added line ends with
MARKER, so applying twice or removing leaves the files exactly as shipped. A file whose anchor lines
changed in an upgrade is left untouched, and reported.
"""
import importlib.util
import sys
from collections import namedtuple
from pathlib import Path

MARKER = "  # howdy-island"
IMPORT_FEED = ['sys.path.insert(0, "/usr/lib/howdy-island")', "import island_feed"]
SEND_FRAME_AND_FACES = ["island_feed.frame(gsframe)", "island_feed.faces(face_locations, gsframe.shape, use_cnn)"]
ADD_MAX_FRAMES = 60

Hook = namedtuple("Hook", ["anchor", "additions", "before"], defaults=[False])

HOWDY_HOOKS_BY_FILE = {
	"compare.py": [
		Hook("from i18n import _", IMPORT_FEED),
		Hook('"""Exit while closing howdy-gtk properly"""', ["island_feed.finish(code)"]),
		Hook("user = sys.argv[1]", ["island_feed.begin(user)"]),
		Hook('send_to_ui("M", _("Identifying you..."))', ["island_feed.scan(timeout, video_certainty * 10)"]),
		Hook("face_locations = face_detector(gsframe, 1)", SEND_FRAME_AND_FACES),
		Hook("match = matches[match_index]", ["island_feed.certainty(match * 10)"]),
	],
	"cli/add.py": [
		Hook("from i18n import _", IMPORT_FEED),
		Hook("video_capture = VideoCapture(config)", ["island_feed.begin_enrolment(user)"]),
		Hook("frames += 1", [f"island_feed.enrolment_progress(frames / {ADD_MAX_FRAMES})"]),
		Hook("face_locations = face_detector(gsframe, 1)", SEND_FRAME_AND_FACES),
		Hook('print(_("Camera saw only black frames - is IR emitter working?"))', ['island_feed.enrolment_finished("black")']),
		Hook('print(_("All frames were too dark, please check dark_threshold in config"))', ['island_feed.enrolment_finished("dark")']),
		Hook('print(_("No face detected, aborting"))', ['island_feed.enrolment_finished("no-face")']),
		Hook('print(_("Multiple faces detected, aborting"))', ['island_feed.enrolment_finished("multiple-faces")']),
		Hook("json.dump(encodings, datafile)", ['island_feed.enrolment_finished("added")']),
	],
}

NWG_HELLO_HOOKS_BY_FILE = {
	"ui.py": [
		Hook('if "error_type" in resp and resp["error_type"] == "auth_error":', [
			'while resp.get("type") == "auth_message" and resp.get("auth_message_type") in ("info", "error"):',
			'    resp = greetd(self.client, {"type": "post_auth_message_response", "response": ""}, log=self.log)',
		], before=True),
	],
}


def package_dir(module):
	spec = importlib.util.find_spec(module)
	return Path(spec.origin).parent if spec and spec.origin else None


TARGETS = {
	"howdy": (lambda: Path("/usr/lib/howdy"), HOWDY_HOOKS_BY_FILE),
	"nwg-hello": (lambda: package_dir("nwg_hello"), NWG_HELLO_HOOKS_BY_FILE),
}


def without_hooks(lines):
	return [line for line in lines if not line.endswith(MARKER)]


def with_hooks(lines, hooks):
	patched = []
	for line in lines:
		indent = line[: len(line) - len(line.lstrip())]
		matching = [hook for hook in hooks if line.strip() == hook.anchor]
		for hook in matching:
			if hook.before:
				patched += [indent + addition + MARKER for addition in hook.additions]
		patched.append(line)
		for hook in matching:
			if not hook.before:
				patched += [indent + addition + MARKER for addition in hook.additions]
	return patched


def missing_anchors(lines, hooks):
	stripped = [line.strip() for line in lines]
	return [hook.anchor for hook in hooks if stripped.count(hook.anchor) != 1]


def patch(path, hooks, remove):
	"""Returns an error message, or None when done."""
	original = without_hooks(path.read_text().splitlines())
	result = original
	if not remove:
		missing = missing_anchors(original, hooks)
		if missing:
			return f"{path}: changed, lines not added. Not found once: " + " | ".join(missing)
		result = with_hooks(original, hooks)

	path.write_text("\n".join(result) + "\n")
	return None


def main():
	if len(sys.argv) < 2 or sys.argv[1] not in TARGETS:
		sys.exit(f"usage: patch_hooks.py {'|'.join(TARGETS)} [--remove]")
	find_dir, hooks_by_file = TARGETS[sys.argv[1]]
	remove = "--remove" in sys.argv
	target_dir = find_dir()
	if target_dir is None or not target_dir.is_dir():
		return

	errors = []
	for relative_path, hooks in hooks_by_file.items():
		error = patch(target_dir / relative_path, hooks, remove)
		if error:
			errors.append(error)

	if errors:
		sys.exit("howdy-island: " + "\nhowdy-island: ".join(errors))


main()
