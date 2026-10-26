#!/bin/sh
# Builds a self-contained r2fuse binary: radare2 (with the GPL grub plugins)
# and libfuse are compiled from source as static libraries and linked in.
#
#   dist/static.sh                 # clone radare2 master
#   R2_SRC=../radare2 dist/static.sh
#   R2_REF=6.2.4 dist/static.sh
#   R2_PREFIX=/usr/local dist/static.sh # use installed r2 release libraries
#
# Linux: run it inside alpine (musl) to get a fully static binary.
# macOS: libfuse is patched with the Fusetta (FSKit) mount backend.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-$ROOT/build-static}
R2_REF=${R2_REF:-master}
R2_SRC=${R2_SRC:-}
R2_PREFIX=${R2_PREFIX:-$WORK/r2}
FUSE_VERSION=3.18.3
FUSE_SHA256=bcd19582c5e30f7fe45dd86a5540e998590aa01903afc7ebcbeea6c8ac5421ee
FUSETTA_REF=${FUSETTA_REF:-main}
OS=$(uname)

mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)

sha256() {
	if command -v sha256sum > /dev/null; then
		sha256sum "$1" | cut -d ' ' -f 1
	else
		shasum -a 256 "$1" | cut -d ' ' -f 1
	fi
}

clone_ref() {
	git init "$2"
	git -C "$2" remote add origin "$1"
	git -C "$2" fetch --depth 1 origin "$3"
	git -C "$2" checkout --detach FETCH_HEAD
}

build_r2() {
	if [ -z "$R2_SRC" ]; then
		R2_SRC=$WORK/radare2-src
		[ -d "$R2_SRC" ] || clone_ref https://github.com/radareorg/radare2 "$R2_SRC" "$R2_REF"
	fi
	rm -rf "$WORK/r2-build"
	meson setup "$WORK/r2-build" "$R2_SRC" --prefix="$WORK/r2" --libdir=lib \
		--buildtype=release --default-library=static \
		-Dcli=disabled -Denable_tests=false
	ninja -C "$WORK/r2-build" install > /dev/null
}

build_fuse() {
	tarball=$WORK/fuse-$FUSE_VERSION.tar.gz
	if [ ! -f "$tarball" ]; then
		curl -fsSL --retry 3 -o "$tarball.part" \
			"https://github.com/libfuse/libfuse/releases/download/fuse-$FUSE_VERSION/fuse-$FUSE_VERSION.tar.gz"
		mv "$tarball.part" "$tarball"
	fi
	if [ "$(sha256 "$tarball")" != "$FUSE_SHA256" ]; then
		echo "checksum mismatch for $tarball" >&2
		rm -f "$tarball"
		exit 1
	fi
	src=$WORK/libfuse-src
	rm -rf "$src" "$WORK/fuse-build"
	mkdir -p "$src"
	tar -xzf "$tarball" -C "$src" --strip-components 1
	extra=
	if [ "$OS" = Darwin ]; then
		[ -d "$WORK/fusetta" ] || clone_ref https://github.com/hsorbo/fusetta "$WORK/fusetta" "$FUSETTA_REF"
		patch -s -d "$src" -p1 < "$WORK/fusetta/patches/libfuse-$FUSE_VERSION-darwin.patch"
		# the darwin mount backend needs the custom io hooks
		extra=-Denable-custom-io=true
	fi
	meson setup "$WORK/fuse-build" "$src" --prefix="$WORK/fuse" --libdir=lib \
		--buildtype=release --default-library=static \
		-Dexamples=false -Dtests=false -Dutils=false -Duseroot=false $extra
	ninja -C "$WORK/fuse-build" install > /dev/null
}

build_r2fuse() {
	make -C "$ROOT" clean > /dev/null
	r2_shared=0
	[ "$R2_PREFIX" = "$WORK/r2" ] || r2_shared=1
	PKG_CONFIG_PATH="$WORK/fuse/lib/pkgconfig:$R2_PREFIX/lib/pkgconfig" \
		make -C "$ROOT" STATIC=1 R2_SHARED="$r2_shared" FUSETTA_PC=
	strip "$ROOT/r2fuse" 2> /dev/null || true
	echo "Built $ROOT/r2fuse"
	case "$OS" in
	Darwin) otool -L "$ROOT/r2fuse" ;;
	*) file "$ROOT/r2fuse" 2> /dev/null || true ;;
	esac
}

[ "$R2_PREFIX" != "$WORK/r2" ] || build_r2
build_fuse
build_r2fuse
