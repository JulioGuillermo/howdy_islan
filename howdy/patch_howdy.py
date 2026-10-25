"""Adds the howdy-island hooks to Howdy's compare.py (authentication) and cli/add.py (howdy add),
or removes them with --remove.

Run again after every Howdy upgrade (the pacman hook does it). Each added line ends with MARKER,
so applying twice or removing leaves the files exactly as Howdy shipped them. A file whose anchor
lines changed in a Howdy upgrade is left untouched, and reported.
"""
import sys
from pathlib import Path

HOWDY_DIR = Path("/usr/lib/howdy")
MARKER = "  # howdy-island"
IMPORT_FEED = ['sys.path.insert(0, "/usr/lib/howdy-island")', "import island_feed"]
SEND_FRAME_AND_FACES = ["island_feed.frame(gsframe)", "island_feed.faces(face_locations, gsframe.shape, use_cnn)"]
ADD_MAX_FRAMES = 60

HOOKS_BY_FILE = {
	"compare.py": [
		("from i18n import _", IMPORT_FEED),
		('"""Exit while closing howdy-gtk properly"""', ["island_feed.finish(code)"]),
		("user = sys.argv[1]", ["island_feed.begin(user)"]),
		('send_to_ui("M", _("Identifying you..."))', ["island_feed.scan(timeout, video_certainty * 10)"]),
		("face_locations = face_detector(gsframe, 1)", SEND_FRAME_AND_FACES),
		("match = matches[match_index]", ["island_feed.certainty(match * 10)"]),
	],
	"cli/add.py": [
		("from i18n import _", IMPORT_FEED),
		("video_capture = VideoCapture(config)", ["island_feed.begin_enrolment(user)"]),
		("frames += 1", [f"island_feed.enrolment_progress(frames / {ADD_MAX_FRAMES})"]),
		("face_locations = face_detector(gsframe, 1)", SEND_FRAME_AND_FACES),
		('print(_("Camera saw only black frames - is IR emitter working?"))', ['island_feed.enrolment_finished("black")']),
		('print(_("All frames were too dark, please check dark_threshold in config"))', ['island_feed.enrolment_finished("dark")']),
		('print(_("No face detected, aborting"))', ['island_feed.enrolment_finished("no-face")']),
		('print(_("Multiple faces detected, aborting"))', ['island_feed.enrolment_finished("multiple-faces")']),
		("json.dump(encodings, datafile)", ['island_feed.enrolment_finished("added")']),
	],
}


def without_hooks(lines):
	return [line for line in lines if not line.endswith(MARKER)]


def with_hooks(lines, hooks):
	patched = []
	for line in lines:
		patched.append(line)
		for anchor, additions in hooks:
			if line.strip() == anchor:
				indent = line[: len(line) - len(line.lstrip())]
				patched += [indent + addition + MARKER for addition in additions]
	return patched


def missing_anchors(lines, hooks):
	stripped = [line.strip() for line in lines]
	return [anchor for anchor, _ in hooks if stripped.count(anchor) != 1]


def patch(path, hooks, remove):
	"""Returns an error message, or None when done."""
	original = without_hooks(path.read_text().splitlines())
	result = original
	if not remove:
		missing = missing_anchors(original, hooks)
		if missing:
			return f"{path}: changed, hooks not added. Lines not found once: " + " | ".join(missing)
		result = with_hooks(original, hooks)

	path.write_text("\n".join(result) + "\n")
	return None


def main():
	remove = "--remove" in sys.argv
	errors = []
	for relative_path, hooks in HOOKS_BY_FILE.items():
		error = patch(HOWDY_DIR / relative_path, hooks, remove)
		if error:
			errors.append(error)

	if errors:
		sys.exit("howdy-island: " + "\nhowdy-island: ".join(errors))


main()
