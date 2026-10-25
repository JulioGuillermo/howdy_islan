"""Turns the howdy-island extension on (or off with --disable) for the current user.

Only adds or removes its own entry in org.gnome.shell enabled-extensions and disabled-extensions;
every other extension stays as it was.
"""
import sys

from gi.repository import Gio

UUID = "howdy-island@julioguillermo.github.io"


def with_uuid(entries):
	if UUID in entries:
		return entries
	return entries + [UUID]


def without_uuid(entries):
	return [entry for entry in entries if entry != UUID]


def update(settings, key, change):
	entries = settings.get_strv(key)
	changed = change(entries)
	if changed != entries:
		settings.set_strv(key, changed)


def main():
	settings = Gio.Settings.new("org.gnome.shell")
	if "--disable" in sys.argv:
		update(settings, "enabled-extensions", without_uuid)
	else:
		update(settings, "enabled-extensions", with_uuid)
		update(settings, "disabled-extensions", without_uuid)
	Gio.Settings.sync()


main()
