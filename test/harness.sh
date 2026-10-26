#!/bin/sh
# Exercise the runner's exit-status handling without real filesystem plugins.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/r2fuse-harness.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
mkdir "$TMP/images"
cat > "$TMP/mock" <<'EOF'
#!/bin/sh
case "$R2FUSE_TEST_STATUS" in
timeout) exec sleep 2 ;;
esac
printf '%s\n' "$R2FUSE_TEST_OUTPUT"
exit "$R2FUSE_TEST_STATUS"
EOF
chmod +x "$TMP/mock"

check() {
	image=$1 status=$2 output=$3 expected=$4
	: > "$TMP/images/$image"
	result=0
	R2FUSE="$TMP/mock" R2FUSE_MOUNT= TIMEOUT=1 \
		R2FUSE_TEST_STATUS="$status" R2FUSE_TEST_OUTPUT="$output" \
		sh "$HERE/run.sh" "$TMP/images" > "$TMP/output" 2>&1 || result=$?
	rm -f "$TMP/images/$image"
	if [ "$result" -ne "$expected" ]; then
		echo "Runner regression: $image, status $status: exit $result, expected $expected"
		cat "$TMP/output"
		exit 1
	fi
}

check ext2.img 0 'valid image' 0
check ext2.img 1 'unexpected read failure' 1
check ext2.img 2 'unexpected mount rejection' 1
check ext2.img 139 'crash' 1
check ext2.img timeout '' 1
check bfs-sibling-cycle.img 2 'malformed image rejected' 0
check bfs-sibling-cycle.img 0 'unexpected acceptance' 1
check bfs-sibling-cycle.img 1 'wrong kind of failure' 1
check bfs-sibling-cycle.img 139 'crash' 1
check ntfs-resident-oob.img 1 'FAIL /file: open failed' 0
check ntfs-resident-oob.img 1 'FAIL /other: open failed' 1
check ntfs-resident-oob.img 1 'FAIL /file: read failed' 1
check ntfs-resident-oob.img 139 'crash' 1
check ntfs-resident-oob.img 0 'unexpected acceptance' 1
echo 'OK    runner rejects unexpected errors, crashes, timeouts and unexpected passes'
