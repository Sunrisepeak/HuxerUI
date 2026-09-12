#!/bin/sh
# Assemble an AppDir out of what `mcpp pack` staged, and run appimagetool on it.
#
# A helper rather than a command line because an action's command is an argv
# with no shell assumed, and this is four steps. Arguments are positional and
# come from huxerui::rules::sources::appimage_arguments, which is unit tested.
set -eu

tool=$1
runtime=$2
stage=$3
appdir=$4
desktop=$5
icon=$6
executable=$7
out=$8

program=$(basename "$executable")

rm -rf "$appdir"
mkdir -p "$appdir"
# The staged tree is a bundle -- bin/, lib/, relocatable -- which is the shape
# an AppDir wants, so it is copied as it stands rather than re-laid out.
cp -a "$stage/." "$appdir/"

# AppRun resolves its own directory rather than trusting $PWD: an AppImage is
# mounted at a path chosen at run time, and the program lives under bin/.
cat > "$appdir/AppRun" <<APPRUN
#!/bin/sh
HERE=\$(dirname "\$(readlink -f "\$0")")
export LD_LIBRARY_PATH="\$HERE/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
exec "\$HERE/bin/$program" "\$@"
APPRUN
chmod +x "$appdir/AppRun"

cp "$desktop" "$appdir/$(basename "$desktop")"
cp "$icon" "$appdir/$(basename "$icon")"

# --runtime-file, ALWAYS: without it appimagetool downloads its type-2 runtime
# stub from a GitHub release on every invocation, and a build that reaches the
# network is neither reproducible nor usable offline.
#
# APPIMAGE_EXTRACT_AND_RUN, because appimagetool is itself an AppImage and
# would otherwise need FUSE, which CI containers do not have.
APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --runtime-file "$runtime" "$appdir" "$out"

# A floor on our own output, on the success path. appimagetool exits 0 for an
# AppDir whose program never arrived, and stderr on a successful build is
# discarded -- so the empty result would otherwise ship silently.
size=$(wc -c < "$out")
if [ "$size" -lt 1048576 ]; then
  echo "huxerui: the AppImage is $size bytes, which does not carry the application" >&2
  exit 1
fi
