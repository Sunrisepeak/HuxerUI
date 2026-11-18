set -eu

if [ -z "${HUXERUI_HOME:-}" ]; then
  echo "error: HUXERUI_HOME is not configured; set it in Config/Local.xcconfig" >&2
  exit 1
fi

# The integration that builds the application core: the project's only declaration, or HUXERUI_BUILD_SYSTEM when
# the project declares both. Everything else in this shell is shared.
if [ -z "${HUXERUI_BUILD_SYSTEM:-}" ]; then
  if [ -f "$HUXERUI_PROJECT_ROOT/CMakeLists.txt" ] && [ -f "$HUXERUI_PROJECT_ROOT/build.mcpp" ]; then
    echo "error: the project declares both cmake and mcpp; set HUXERUI_BUILD_SYSTEM in Config/Local.xcconfig" >&2
    exit 1
  elif [ -f "$HUXERUI_PROJECT_ROOT/build.mcpp" ]; then
    HUXERUI_BUILD_SYSTEM=mcpp
  else
    HUXERUI_BUILD_SYSTEM=cmake
  fi
fi

if [ "$HUXERUI_BUILD_SYSTEM" = mcpp ]; then
  # mcpp builds the core for arm64; its archive carries the C++ runtime the core was compiled against, and the
  # shell calls the core only through huxerui::RunApplication(), whose signature names no standard-library type.
  case "$PLATFORM_NAME" in
    iphonesimulator) HUXERUI_MCPP_TARGET=aarch64-ios-sim ;;
    iphoneos) HUXERUI_MCPP_TARGET=aarch64-ios ;;
    *) echo "error: the mcpp integration builds no core for $PLATFORM_NAME" >&2; exit 1 ;;
  esac
  for HUXERUI_ARCH in $ARCHS; do
    if [ "$HUXERUI_ARCH" != arm64 ]; then
      echo "error: the mcpp integration builds the iOS core for arm64 only, not $HUXERUI_ARCH" >&2
      exit 1
    fi
  done
  HUXERUI_MCPP_PROFILE=
  if [ "$CONFIGURATION" = Release ]; then
    HUXERUI_MCPP_PROFILE=--release
  fi
  mkdir -p "$HUXERUI_CORE_BUILD_DIR/huxerui-ios"
  HUXERUI_MCPP_REPORT="$HUXERUI_CORE_BUILD_DIR/mcpp-build.json"
  (cd "$HUXERUI_PROJECT_ROOT" && "${HUXERUI_MCPP:-mcpp}" build --target "$HUXERUI_MCPP_TARGET" $HUXERUI_MCPP_PROFILE \
    --message-format json) > "$HUXERUI_MCPP_REPORT"
  HUXERUI_MCPP_PROGRAM=$(grep '"event":"artifact"' "$HUXERUI_MCPP_REPORT" |
    sed -n 's/.*"path":"\([^"]*\)".*/\1/p' | head -n 1)
  HUXERUI_MCPP_CORE=$(dirname "$HUXERUI_MCPP_PROGRAM")/huxerui-ios
  set -- "$HUXERUI_MCPP_CORE"/*/
  if [ "$#" -ne 1 ] || [ ! -f "$1/link.rsp" ]; then
    echo "error: mcpp produced no application core under $HUXERUI_MCPP_CORE" >&2
    exit 1
  fi
  # The core keeps the location mcpp gave it; the shell reads it where it reads a CMake core.
  rm -rf "$HUXERUI_CORE_BUILD_DIR/huxerui-ios/@TARGET_NAME@"
  ln -s "${1%/}" "$HUXERUI_CORE_BUILD_DIR/huxerui-ios/@TARGET_NAME@"
  exit 0
fi

HUXERUI_CMAKE_ARCHS=$(printf '%s' "$ARCHS" | tr ' ' ';')
cmake -S "$HUXERUI_PROJECT_ROOT" -B "$HUXERUI_CORE_BUILD_DIR" \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT="$SDKROOT" \
  -DCMAKE_OSX_ARCHITECTURES="$HUXERUI_CMAKE_ARCHS" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$IPHONEOS_DEPLOYMENT_TARGET" \
  -DCMAKE_BUILD_TYPE="$CONFIGURATION" \
  -DHUXERUI_HOME="$HUXERUI_HOME" \
  -DHUXERUI_OPTIMIZATION="${HUXERUI_OPTIMIZATION:-default}" \
  -DHUXERUI_ENABLE_LTO="${HUXERUI_ENABLE_LTO:-OFF}" \
  -DHUXERUI_ENABLE_PROFILING="${HUXERUI_ENABLE_PROFILING:-OFF}" \
  -DHUXERUI_BUILD_SHARED=OFF \
  -DHUXERUI_BUILD_TESTS=OFF \
  -DHUXERUI_BUILD_EXAMPLES=OFF \
  -DHUXERUI_BUILD_CLI=OFF
cmake --build "$HUXERUI_CORE_BUILD_DIR" --target @TARGET_NAME@_huxerui_ios_core --parallel
