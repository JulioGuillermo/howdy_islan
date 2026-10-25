"""Adds the howdy-island hooks to Howdy's compare.py, or removes them with --remove.

Run again after every Howdy upgrade (the pacman hook does it). Each added line ends with MARKER,
so applying twice or removing leaves the file exactly as Howdy shipped it.
"""
import sys
from pathlib import Path

COMPARE_PATH = Path("/usr/lib/howdy/compare.py")
MARKER = "  # howdy-island"
HOOKS = [
	("from i18n import _", ['sys.path.insert(0, "/usr/lib/howdy-island")', "import island_feed"]),
	('"""Exit while closing howdy-gtk properly"""', ["island_feed.finish(code)"]),
	("user = sys.argv[1]", ["island_feed.begin(user)"]),
	('send_to_ui("M", _("Identifying you..."))', ["island_feed.scan(timeout, video_certainty * 10)"]),
	("face_locations = face_detector(gsframe, 1)", ["island_feed.frame(gsframe)", "island_feed.faces(face_locations, gsframe.shape, use_cnn)"]),
	("match = matches[match_index]", ["island_feed.certainty(match * 10)"]),
]


def without_hooks(lines):
	return [line for line in lines if not line.endswith(MARKER)]


def with_hooks(lines):
	patched = []
	for line in lines:
		patched.append(line)
		for anchor, additions in HOOKS:
			if line.strip() == anchor:
				indent = line[: len(line) - len(line.lstrip())]
				patched += [indent + addition + MARKER for addition in additions]
	return patched


def missing_anchors(lines):
	stripped = [line.strip() for line in lines]
	return [anchor for anchor, _ in HOOKS if stripped.count(anchor) != 1]


def main():
	original = without_hooks(COMPARE_PATH.read_text().splitlines())

	if "--remove" in sys.argv:
		result = original
	else:
		missing = missing_anchors(original)
		if missing:
			sys.exit("howdy-island: compare.py changed, hooks not added. Lines not found once: " + " | ".join(missing))
		result = with_hooks(original)

	COMPARE_PATH.write_text("\n".join(result) + "\n")


main()
