# Shell helpers shared by the install scripts. Source it, do not run it.

PAM_SERVICES=${PAM_SERVICES:-"sudo polkit-1 system-local-login"}

die() {
	echo "ERROR: $*" >&2
	exit 1
}

# The first video device with an 8-bit gray format: the IR camera.
find_ir_camera() {
	local device
	for device in /dev/video*; do
		if v4l2-ctl -d "$device" --list-formats 2>/dev/null | grep -q "'GREY'"; then
			echo "$device"
			return
		fi
	done
}

pam_block_start() {
	echo "# >>> $1 (install_$1)"
}

pam_block_end() {
	echo "# <<< $1 (install_$1)"
}

# Whether any of PAM_SERVICES carries the block of this backend.
has_pam_block() {
	local backend=$1 service
	for service in $PAM_SERVICES; do
		if grep -qxF "$(pam_block_start "$backend")" "/etc/pam.d/$service" 2>/dev/null; then
			return 0
		fi
	done
	return 1
}

remove_pam_block() {
	local backend=$1 file=$2
	sudo sed -i "/^$(pam_block_start "$backend")\$/,/^$(pam_block_end "$backend")\$/d" "$file"
}

# Puts the lines, between the backend's markers, at the top of the auth stack of the file.
add_pam_block() {
	local backend=$1 file=$2 lines=$3
	local block
	block="$(pam_block_start "$backend")
$lines
$(pam_block_end "$backend")"

	sudo cp -n "$file" "$file.bak-$backend"
	remove_pam_block "$backend" "$file"

	local patched
	patched=$(mktemp)
	if head -1 "$file" | grep -q '^#%PAM'; then
		{ head -1 "$file"; echo "$block"; tail -n +2 "$file"; } >"$patched"
	else
		{ echo "$block"; cat "$file"; } >"$patched"
	fi
	sudo install -m 644 -o root -g root "$patched" "$file"
	rm "$patched"
}

add_pam_blocks() {
	local backend=$1 lines=$2 service
	for service in $PAM_SERVICES; do
		[[ -f /etc/pam.d/$service ]] || continue
		add_pam_block "$backend" "/etc/pam.d/$service" "$lines"
		echo "PAM: $backend added to /etc/pam.d/$service (backup in $service.bak-$backend)"
	done
}

remove_pam_blocks() {
	local backend=$1 service
	for service in $PAM_SERVICES; do
		[[ -f /etc/pam.d/$service ]] || continue
		remove_pam_block "$backend" "/etc/pam.d/$service"
		echo "PAM: $backend removed from /etc/pam.d/$service"
	done
}

is_package_installed() {
	pacman -Q "$1" >/dev/null 2>&1
}
