#!/bin/sh
# Runs `r2fuse -s` (walk + read stress, no kernel involved) over the images
# listed in test/images.txt. Set R2FUSE_MOUNT=1 to also mount each passing
# image and compare the files read through the kernel with the manifest.
#
#   test/run.sh [testbins/fs directory]

TESTBINS=${1:-../radare2/test/bins/fs}
HERE=$(cd "$(dirname "$0")" && pwd)
R2FUSE=${R2FUSE:-$HERE/../r2fuse}
TIMEOUT=${TIMEOUT:-60}
TMP=${TMPDIR:-/tmp}/r2fuse-test.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

if [ ! -d "$TESTBINS" ]; then
	echo "Cannot find the test images directory: $TESTBINS" >&2
	exit 1
fi

with_timeout() {
	perl -e 'alarm shift; exec @ARGV or die "exec: $!"' "$TIMEOUT" "$@"
}

mount_test() {
	img=$1; shift
	mnt=$TMP/mnt
	mkdir -p "$mnt"
	"$R2FUSE" "$@" "$img" "$mnt" || return 1
	i=0
	while [ $i -lt 50 ] && ! mount | grep -q " $mnt "; do
		sleep 0.1; i=$((i + 1))
	done
	rc=0
	# every regular file listed in the manifest must have the same size via the kernel
	grep '^f ' "$TMP/out" | while read -r _ size _ path; do
		ksize=$(wc -c < "$mnt$path" | tr -d ' ')
		if [ "$ksize" != "$size" ]; then
			echo "  mount: $path size $ksize != $size"
			echo x >> "$TMP/mountfail"
		fi
	done
	[ -f "$TMP/mountfail" ] && rc=1
	rm -f "$TMP/mountfail"
	umount "$mnt" 2>/dev/null || fusermount3 -u "$mnt" 2>/dev/null || fusermount -u "$mnt" 2>/dev/null
	return $rc
}

pass=0; fail=0; xfail=0; xpass=0; skip=0
while read -r img fstype expect notes; do
	case "$img" in ''|'#'*) continue ;; esac
	path=$TESTBINS/$img
	if [ ! -f "$path" ]; then
		echo "SKIP  $img (missing)"
		skip=$((skip + 1))
		continue
	fi
	set --
	[ "$fstype" != "-" ] && set -- -t "$fstype"
	rc=0
	with_timeout "$R2FUSE" -s "$@" "$path" > "$TMP/out" 2>&1 || rc=$?
	case "$rc" in
	0)
		res=ok
		if [ -n "$R2FUSE_MOUNT" ] && ! mount_test "$path" "$@"; then
			res=fail
		fi
		;;
	1) res=fail ;;
	2) res=reject ;;
	*) res=crash ;;
	esac
	case "$expect" in
	reject-file:*)
		if [ "$rc" -eq 1 ] && [ "$(grep -c '^FAIL ' "$TMP/out")" -eq 1 ] \
				&& grep -Fxq "FAIL ${expect#reject-file:}: open failed" "$TMP/out"; then
			res=$expect
		fi
		;;
	esac
	summary=$(tail -n 1 "$TMP/out")
	if [ "$res" != crash ] && [ "$res" = "$expect" ]; then
		if [ "$res" = ok ]; then
			echo "OK    $img: $summary"; pass=$((pass + 1))
		elif [ "$res" != fail ]; then
			echo "OK    $img: rejected malformed image or file"; pass=$((pass + 1))
		else
			echo "XFAIL $img: $notes"; xfail=$((xfail + 1))
		fi
	elif [ "$res" = ok ]; then
		echo "XPASS $img: $summary (update test/images.txt)"; xpass=$((xpass + 1))
	else
		echo "FAIL  $img: exit $rc (expected $expect): $summary"
		grep -E '^(FAIL|ERROR)' "$TMP/out" | head -n 5 | sed 's/^/      /'
		fail=$((fail + 1))
	fi
done < "$HERE/images.txt"

echo "# ok=$pass fail=$fail xfail=$xfail xpass=$xpass skip=$skip"
[ $fail -eq 0 ] && [ $xpass -eq 0 ]
