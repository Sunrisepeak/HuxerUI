#!/usr/bin/env bash
# program_model_facts.sh <platform> <project directory>
#
# Reads what `huxerui package <platform>` published to <project>/dist/<platform>
# and prints the program model facts docs/design/build-systems-spec.md names,
# one `<platform>.<fact>=<value>` per line, sorted. Two projects built by two
# build systems are compared with compare_program_model.sh.
#
# A fact describes the program a user gets, not how it was built: the artifact,
# its identity and icon, its platform metadata, what it links, and every file of
# its resource package with a digest. A fact is printed whether or not the other
# build system has it, so a key only one of them writes is a difference too.
set -euo pipefail

platform=${1:?usage: program_model_facts.sh <platform> <project directory>}
project=${2:?usage: program_model_facts.sh <platform> <project directory>}
[ -d "$project/dist/$platform" ] || { echo "no $project/dist/$platform: huxerui package $platform published nothing" >&2; exit 1; }
# Absolute, because the AppImage is run from a scratch directory.
dist="$(cd "$project/dist/$platform" && pwd)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
facts="$work/facts.txt"
: > "$facts"

fact() { printf '%s.%s=%s\n' "$platform" "$1" "$2" >> "$facts"; }
# `key=value` lines a helper wrote, under this platform's prefix.
facts_from() { sed "s/^/$platform./" "$1" >> "$facts"; }

sha() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# Every file under a resource package root, by its path in the package.
resource_facts() {
  local root=$1
  [ -d "$root" ] || { fact resources.present no; return; }
  fact resources.present yes
  (cd "$root" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) | while IFS= read -r f; do
    fact "resources.file.$f" "$(sha "$root/$f")"
  done
}

artifacts() { find "$dist" -mindepth 1 -maxdepth 1 -name "$1" | LC_ALL=C sort; }

is_elf() { [ "$(head -c 4 "$1" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]; }

# An Info.plist's entries, minus the ones that name the machine that built it.
plist_facts() {
  plutil -convert json -o "$work/plist.json" "$1"
  python3 - "$work/plist.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
# The launch screen and the icon are declared either way; which form each takes is its own fact.
if "UILaunchStoryboardName" in d or "UILaunchScreen" in d:
    print("plist.launch_screen=declared")
    print("plist.launch_screen_form=" + ("storyboard" if "UILaunchStoryboardName" in d else "dictionary"))
elif "UIDeviceFamily" in d:
    print("plist.launch_screen=missing")
icon_keys = ("CFBundleIcons", "CFBundleIcons~ipad", "CFBundleIconFiles", "CFBundleIconFile", "CFBundleIconName")
print("plist.icon=" + ("declared" if any(k in d for k in icon_keys) else "missing"))
for k in sorted(d):
    if k in ("UILaunchStoryboardName", "UILaunchScreen", "BuildMachineOSBuild") or k in icon_keys or k.startswith("DT"):
        continue
    v = d[k]
    print(f"plist.{k}=" + (json.dumps(v, sort_keys=True) if not isinstance(v, str) else v))
PY
}

case "$platform" in
  linux)
    appimage=$(artifacts '*.AppImage' | head -1)
    [ -n "$appimage" ] || { echo "no AppImage in $dist" >&2; exit 1; }
    fact artifact.kind appimage
    fact artifact.count "$(artifacts '*' | wc -l | tr -d ' ')"
    fact artifact.name "$(basename "$appimage")"
    chmod +x "$appimage"
    (cd "$work" && "$appimage" --appimage-extract >/dev/null)
    root="$work/squashfs-root"
    desktop=$(find "$root" -maxdepth 1 -name '*.desktop' | head -1)
    [ -n "$desktop" ] || { echo "the AppImage carries no desktop entry" >&2; exit 1; }
    sed -n '/^\[Desktop Entry\]/,/^\[/{/^[A-Za-z][A-Za-z0-9-]*=/p;}' "$desktop" | LC_ALL=C sort | sed 's/^/desktop./' > "$work/linux.txt"
    facts_from "$work/linux.txt"
    icon_name=$(sed -n 's/^Icon=//p' "$desktop" | head -1)
    icon=$(find "$root" -maxdepth 1 -type f \( -name "$icon_name.png" -o -name "$icon_name.svg" -o -name "$icon_name.svgz" -o -name "$icon_name.xpm" \) | LC_ALL=C sort | head -1)
    if [ -n "$icon" ]; then
      fact icon.format "${icon##*.}"
      fact icon.sha256 "$(sha "$icon")"
    else
      fact icon.format none
    fi
    # The desktop entry names a launcher; the program is the ELF file of that name.
    exec_name=$(sed -n 's/^Exec=//p' "$desktop" | head -1 | cut -d' ' -f1)
    exe=
    while IFS= read -r candidate; do
      if is_elf "$candidate"; then exe=$candidate; break; fi
    done < <(find "$root" -type f -name "$exec_name" | LC_ALL=C sort)
    fact executable.found "$([ -n "$exe" ] && echo yes || echo no)"
    if [ -n "$exe" ]; then
      readelf -d "$exe" > "$work/dynamic.txt"
      fact framework.linkage "$(grep -q 'NEEDED.*libhuxerui' "$work/dynamic.txt" && echo shared || echo static)"
      # Where GTK comes from at run time: the host's distribution, or a copy inside the image.
      fact runtime.gtk "$([ -n "$(find "$root" -name 'libgtk-4.so*' -print -quit)" ] && echo bundled || echo system)"
      fact binary.executable "$(sha "$exe")"
      resource_facts "$(dirname "$exe")/$exec_name.resources"
    fi
    ;;

  android)
    apk=$(artifacts '*.apk' | head -1)
    [ -n "$apk" ] || { echo "no APK in $dist" >&2; exit 1; }
    fact artifact.kind apk
    fact artifact.count "$(artifacts '*' | wc -l | tr -d ' ')"
    fact artifact.name "$(basename "$apk")"
    # The Android SDK's build tools when the runner has them, else mcpp's payload.
    tool() { { find "${ANDROID_HOME:-/nonexistent}/build-tools" "$HOME/.mcpp/registry/data/xpkgs" -name "$1" -type f 2>/dev/null || true; } | LC_ALL=C sort | tail -1; }
    aapt2=$(tool aapt2)
    apksigner=$(tool apksigner)
    dexdump=$(tool dexdump)
    [ -n "$aapt2" ] || { echo "no aapt2 to read the APK with" >&2; exit 1; }
    "$aapt2" dump badging "$apk" > "$work/badging.txt"
    "$aapt2" dump xmltree "$apk" --file AndroidManifest.xml > "$work/manifest.txt"
    "$aapt2" dump resources "$apk" > "$work/resources.txt"
    python3 - "$work/badging.txt" "$work/manifest.txt" "$work/resources.txt" > "$work/android.txt" <<'PY'
import re, sys
badging = open(sys.argv[1], encoding="utf-8", errors="replace").read()
def one(pattern):
    m = re.search(pattern, badging, re.M)
    return m.group(1) if m else ""
print("manifest.package=" + one(r"^package: name='([^']*)'"))
print("version.code=" + one(r"versionCode='([^']*)'"))
print("version.name=" + one(r"versionName='([^']*)'"))
print("manifest.min_sdk=" + one(r"^(?:min)?[sS]dkVersion:'([^']*)'"))
print("manifest.target_sdk=" + one(r"^targetSdkVersion:'([^']*)'"))
print("manifest.label=" + one(r"^application-label:'([^']*)'"))
print("manifest.launchable_activity=" + one(r"^launchable-activity: name='([^']*)'"))
print("abis=" + " ".join(sorted(re.findall(r"'([^']+)'", one(r"^native-code:(.*)$")))))
print("manifest.permissions=" + ",".join(sorted(set(re.findall(r"^uses-permission: name='([^']*)'", badging, re.M)))))
# Whether the launcher icon is adaptive. Not its densities: the Android Gradle
# plugin shortens a release package's resource paths, which changes what aapt2
# resolves for a density the package has no file for.
icons = re.findall(r"^application-icon-(\d+):'([^']*)'", badging, re.M)
print("icon.adaptive=" + ("yes" if any(p.endswith(".xml") for _, p in icons) else "no"))
# An application resource is named, not numbered: the two build systems assign
# identifiers independently.
names = {}
for line in open(sys.argv[3], encoding="utf-8", errors="replace"):
    m = re.match(r"\s*resource (0x[0-9a-f]{8}) (\S+)", line)
    if m:
        names[m.group(1)] = m.group(2)
def named(value):
    return re.sub(r"@(0x7f[0-9a-f]{6})", lambda m: "@" + names.get(m.group(1), "unknown"), value)
# The <application> and <activity> attributes, by name, from the compiled manifest.
element, attributes = None, {}
for line in open(sys.argv[2], encoding="utf-8", errors="replace"):
    e = re.match(r"\s*E: (\S+)", line)
    if e:
        element = e.group(1)
        continue
    a = re.match(r"\s*A: http://schemas.android.com/apk/res/android:(\w+)\([^)]*\)=(.*)$", line)
    if a and element in ("application", "activity"):
        value = re.sub(r'\s*\(Raw: "[^"]*"\)$', "", a.group(2)).strip().strip('"')
        attributes.setdefault(element, {})[a.group(1)] = named(value)
for element in ("application", "activity"):
    for name, value in sorted(attributes.get(element, {}).items()):
        if element == "activity" and name == "name":
            continue
        print(f"manifest.{element}.{name}={value}")
PY
    facts_from "$work/android.txt"
    unzip -Z1 "$apk" > "$work/entries.txt"
    for abi in $(grep -oE '^lib/[^/]+/' "$work/entries.txt" | cut -d/ -f2 | LC_ALL=C sort -u); do
      fact "libs.$abi" "$(grep -E "^lib/$abi/[^/]+\.so$" "$work/entries.txt" | sed "s|^lib/$abi/||" | LC_ALL=C sort | paste -sd, -)"
    done
    # How each native library travels: stored uncompressed (loaded from the APK in place) or
    # compressed, and with or without its symbol table and debug information.
    unzip -v "$apk" > "$work/listing.txt"
    mkdir -p "$work/apk"
    unzip -q "$apk" 'lib/*' -d "$work/apk" 2>/dev/null || true
    while IFS= read -r so; do
      method=$(awk -v name="$so" '$NF == name { print $2 }' "$work/listing.txt")
      fact "lib.${so#lib/}.packaging" "$([ "$method" = Stored ] && echo stored || echo compressed)"
      readelf -S "$work/apk/$so" > "$work/sections.txt" 2>/dev/null || true
      if grep -q '\.debug_' "$work/sections.txt"; then debug=debug-info
      elif grep -q '\.symtab' "$work/sections.txt"; then debug=symbols
      else debug=stripped; fi
      fact "lib.${so#lib/}.debug" "$debug"
    done < <(grep -E '^lib/[^/]+/[^/]+\.so$' "$work/entries.txt" | LC_ALL=C sort)
    if grep -qE '^META-INF/.*\.(RSA|EC|DSA)$' "$work/entries.txt"; then
      if [ -n "$apksigner" ] && "$apksigner" verify --print-certs "$apk" > "$work/certs.txt" 2>/dev/null \
         && grep -q 'CN=Android Debug' "$work/certs.txt"; then
        fact signature debug
      else
        fact signature other
      fi
    else
      fact signature unsigned
    fi
    unzip -q "$apk" 'classes*.dex' 'assets/*' -d "$work/apk" 2>/dev/null || true
    # The named Java and Kotlin classes the program carries, one fact each. A
    # compiler's own classes are left out -- anonymous and synthetic classes
    # (`$1`, `$$ExternalSyntheticLambda0`, `-IA`) and the platform API outlines
    # and backports D8 copies in -- because each compiler names them its own way.
    if [ -n "$dexdump" ]; then
      for dex in "$work"/apk/classes*.dex; do
        if [ -f "$dex" ]; then "$dexdump" "$dex" 2>/dev/null || true; fi
      done | sed -n "s/^ *Class descriptor *: 'L\(.*\);'$/\1/p" | tr / . \
        | grep -vE '\$\$|\$[0-9]+(\$|$)|-IA$|^(android|java|javax|com\.android\.tools)\.' | LC_ALL=C sort -u > "$work/classes.txt" || true
      while IFS= read -r class; do fact "dex.class.$class" present; done < "$work/classes.txt"
    fi
    resource_facts "$work/apk/assets"
    ;;

  web)
    js=$(artifacts '*.js' | head -1)
    [ -n "$js" ] || { echo "no launcher script in $dist" >&2; exit 1; }
    name=$(basename "$js" .js)
    fact artifact.kind web
    fact launcher.name "$name"
    # File names with the launcher's stem written as <name>, so the set is compared, not the stem; the
    # page is page.file below.
    fact artifact.files "$(artifacts '*' | sed "s|.*/||; s|^$name\.|<name>.|" | grep -vxE 'index\.html|<name>\.html' | LC_ALL=C sort | paste -sd, -)"
    while IFS= read -r f; do
      case "$f" in *.js|*.wasm|*.data|*.map|*.html) continue ;; esac
      if [ -f "$f" ]; then fact "file.$(basename "$f" | sed "s|^$name\.|<name>.|")" "$(sha "$f")"; fi
    done < <(artifacts '*')
    # dist-web names the page index.html, CMake names it after the target.
    page=$(artifacts 'index.html' | head -1)
    [ -n "$page" ] || page=$(artifacts "$name.html" | head -1)
    fact page.present "$([ -n "$page" ] && echo yes || echo no)"
    if [ -n "$page" ]; then
      fact page.file "$(basename "$page" | sed "s|^$name\.|<name>.|")"
      python3 - "$page" "$name" > "$work/web.txt" <<'PY'
import re, sys
text, name = open(sys.argv[1], encoding="utf-8").read(), sys.argv[2]
stem = lambda s: s.replace(name + ".", "<name>.")
m = re.search(r"<title>(.*?)</title>", text, re.S)
print("page.title=" + (m.group(1).strip() if m else ""))
for rel, href in sorted(re.findall(r'<link\s+rel="([^"]+)"[^>]*?href="([^"]+)"', text)):
    print(f"page.link.{rel}=" + stem(href))
m = re.search(r'import\s+\w+\s+from\s+"([^"]+)"', text)
print("page.module=" + (stem(m.group(1)) if m else ""))
m = re.search(r'huxeruiStorageKey:\s*"([^"]*)"', text)
print("page.storage_key=" + (m.group(1) if m else ""))
PY
      facts_from "$work/web.txt"
    fi
    data=$(artifacts '*.data' | head -1)
    if [ -z "$data" ]; then
      fact resources.present no
    else
      # The preloaded files are named in the launcher's loadPackage() metadata:
      # JSON from one Emscripten, a minified object literal from another.
      python3 - "$js" "$data" "$work/preload" <<'PY'
import os, re, sys
text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
data = open(sys.argv[2], "rb").read()
entry = re.compile(r'\{\s*"?filename"?\s*:\s*"([^"]+)"\s*,\s*"?start"?\s*:\s*(\d+)\s*,\s*"?end"?\s*:\s*(\d+)')
for filename, start, end in entry.findall(text):
    out = os.path.join(sys.argv[3], filename.lstrip("/"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "wb").write(data[int(start):int(end)])
PY
      resource_facts "$work/preload"
    fi
    ;;

  macos)
    dmg=$(artifacts '*.dmg' | head -1)
    [ -n "$dmg" ] || { echo "no DMG in $dist" >&2; exit 1; }
    fact artifact.kind dmg
    fact artifact.count "$(artifacts '*' | wc -l | tr -d ' ')"
    fact artifact.name "$(basename "$dmg")"
    mount="$work/mount"
    mkdir -p "$mount"
    hdiutil attach -nobrowse -readonly -mountpoint "$mount" "$dmg" >/dev/null
    trap 'hdiutil detach "$mount" >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
    fact dmg.entries "$(find "$mount" -mindepth 1 -maxdepth 1 ! -name '.*' -exec basename {} \; | LC_ALL=C sort | paste -sd, -)"
    fact dmg.applications_link "$([ -L "$mount/Applications" ] && echo yes || echo no)"
    app=$(find "$mount" -maxdepth 1 -name '*.app' | head -1)
    [ -n "$app" ] || { echo "the DMG carries no .app" >&2; exit 1; }
    fact app.name "$(basename "$app")"
    plist="$app/Contents/Info.plist"
    plist_facts "$plist" > "$work/macos.txt"
    facts_from "$work/macos.txt"
    # Without CFBundleExecutable the system runs the executable named after the bundle.
    exe_name=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$plist" 2>/dev/null || basename "$app" .app)
    exe="$app/Contents/MacOS/$exe_name"
    icon_file=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIconFile' "$plist" 2>/dev/null || true)
    if [ -n "$icon_file" ]; then
      icon="$app/Contents/Resources/$icon_file"
      [ -f "$icon" ] || icon="$icon.icns"
      fact icon.sha256 "$(if [ -f "$icon" ]; then sha "$icon"; else echo missing; fi)"
    fi
    otool -l "$exe" > "$work/load.txt"
    otool -L "$exe" > "$work/libs.txt"
    fact minos "$(grep -A4 LC_BUILD_VERSION "$work/load.txt" | sed -n 's/.*minos //p' | head -1)"
    fact runtime.libcxx "$(grep -q 'libc++' "$work/libs.txt" && echo system || echo static)"
    fact framework.linkage "$(grep -q 'libhuxerui' "$work/libs.txt" && echo shared || echo static)"
    if [ -d "$app/Contents/Frameworks" ]; then
      fact frameworks "$(find "$app/Contents/Frameworks" -mindepth 1 -maxdepth 1 -exec basename {} \; | LC_ALL=C sort | paste -sd, -)"
    else
      fact frameworks none
    fi
    fact codesign.verify "$(codesign --verify --deep --strict "$app" >/dev/null 2>&1 && echo ok || echo failed)"
    fact binary.executable "$(sha "$exe")"
    resource_facts "$app/Contents/Resources/HuxerUI"
    ;;

  ios)
    app=$(artifacts '*.app' | head -1)
    [ -n "$app" ] || { echo "no .app in $dist" >&2; exit 1; }
    fact artifact.kind app
    fact app.name "$(basename "$app")"
    plist_facts "$app/Info.plist" > "$work/ios.txt"
    facts_from "$work/ios.txt"
    fact icon.container "$([ -f "$app/Assets.car" ] && echo asset-catalog || echo png)"
    exe_name=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Info.plist" 2>/dev/null || basename "$app" .app)
    exe="$app/$exe_name"
    otool -L "$exe" > "$work/libs.txt"
    fact runtime.libcxx "$(grep -q 'libc++' "$work/libs.txt" && echo system || echo static)"
    fact framework.linkage "$(grep -q 'libhuxerui' "$work/libs.txt" && echo shared || echo static)"
    fact binary.executable "$(sha "$exe")"
    resource_facts "$app/HuxerUI"
    ;;

  windows)
    setup=$(artifacts '*.exe' | head -1)
    [ -n "$setup" ] || { echo "no Setup.exe in $dist" >&2; exit 1; }
    fact artifact.kind setup
    fact artifact.count "$(artifacts '*' | wc -l | tr -d ' ')"
    fact artifact.name "$(basename "$setup")"
    winpath() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi; }
    python=$(command -v python || command -v python3)
    # The interface container and the manifest that names its payloads, read with 7-Zip.
    sevenzip=$(command -v 7z || true)
    [ -n "$sevenzip" ] || [ ! -x "/c/Program Files/7-Zip/7z.exe" ] || sevenzip="/c/Program Files/7-Zip/7z.exe"
    [ -n "$sevenzip" ] || { echo "no 7-Zip to read the bundle with" >&2; exit 1; }
    mkdir -p "$work/ux"
    (cd "$work/ux" && "$sevenzip" x -y "$(winpath "$setup")" > /dev/null)
    [ -f "$work/ux/0" ] || { echo "7-Zip found no Burn manifest in $setup" >&2; exit 1; }
    "$python" - "$work/ux" > "$work/bundle.txt" <<'PY'
import hashlib, os, sys, xml.etree.ElementTree as ET
ux = sys.argv[1]
root = ET.fromstring(open(os.path.join(ux, "0"), "rb").read().decode("utf-8-sig"))
tag = lambda e: e.tag.split("}")[1]
for el in root:
    t = tag(el)
    if t == "Variable":
        print(f"bundle.variable.{el.get('Id')}={el.get('Value')}")
    elif t == "RelatedBundle":
        print(f"bundle.upgrade_code={el.get('Id')}")
    elif t == "Registration":
        print(f"bundle.registration.executable={el.get('ExecutableName')}")
        print(f"bundle.registration.per_machine={el.get('PerMachine')}")
        print(f"bundle.registration.version={el.get('Version')}")
    elif t == "UX":
        # A payload's content where the program reads it (its resources); a
        # program, a library or a generated data file only by its presence.
        for payload in el:
            path = payload.get("FilePath").replace("\\", "/")
            source = os.path.join(ux, payload.get("SourcePath"))
            value = "present"
            if ".resources/" in path:
                value = hashlib.sha256(open(source, "rb").read()).hexdigest()
            print(f"bundle.payload.{path}={value}")
    elif t == "Chain":
        for package in el:
            if tag(package) != "MsiPackage":
                continue
            print(f"bundle.msi.upgrade_code={package.get('UpgradeCode')}")
            print(f"bundle.msi.version={package.get('Version')}")
            for child in package:
                if tag(child) == "MsiProperty":
                    print(f"bundle.msi.property.{child.get('Id')}={child.get('Value')}")
                elif tag(child) == "Provides":
                    print(f"bundle.msi.display_name={child.get('DisplayName')}")
PY
    facts_from "$work/bundle.txt"
    # The MSI the bundle chains, taken out by WiX and installed administratively,
    # which lays its files out without registering anything.
    wix=$(command -v wix 2>/dev/null || { find "$HOME/.mcpp/registry/data/xpkgs" -iname wix.exe -type f 2>/dev/null || true; } | LC_ALL=C sort | tail -1)
    if [ -n "$wix" ]; then
      "$wix" burn extract "$(winpath "$setup")" -o "$(winpath "$work/payloads")" > /dev/null
      msi="$work/product.msi"
      cp "$(find "$work/payloads" -type f | head -1)" "$msi"
      "$wix" msi decompile "$(winpath "$msi")" -o "$(winpath "$work/product.wxs")" > /dev/null
      printf '%s\n' "Start-Process msiexec.exe -Wait -ArgumentList @('/a', '\"$(winpath "$msi")\"', '/qn', 'TARGETDIR=\"$(winpath "$work/image")\"')" > "$work/admin.ps1"
      powershell -NoProfile -ExecutionPolicy Bypass -File "$(winpath "$work/admin.ps1")"
      "$python" - "$work/product.wxs" "$work/image" > "$work/msi.txt" <<'PY'
import hashlib, os, sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
tag = lambda e: e.tag.split("}")[-1]
for el in root.iter():
    t = tag(el)
    if t == "Package":
        print(f"msi.name={el.get('Name')}")
        print(f"msi.manufacturer={el.get('Manufacturer')}")
        print(f"msi.scope={el.get('Scope', 'perMachine')}")
    elif t == "Shortcut":
        print(f"msi.shortcut.{el.get('Directory')}/{el.get('Name')}=present")
    elif t == "Property" and el.get("Id", "").startswith("ARP"):
        print(f"msi.property.{el.get('Id')}={el.get('Value')}")
# The installed files, relative to the program's folder.
image, program = sys.argv[2], None
for base, _, files in os.walk(image):
    if any(f.lower().endswith(".exe") for f in files) and program is None:
        program = base
if program:
    for base, _, files in os.walk(program):
        for f in sorted(files):
            full = os.path.join(base, f)
            rel = os.path.relpath(full, program).replace("\\", "/")
            if rel.lower().endswith(".msi"):
                continue
            value = hashlib.sha256(open(full, "rb").read()).hexdigest() if ".resources/" in rel else "present"
            print(f"msi.file.{rel}={value}")
PY
      facts_from "$work/msi.txt"
    fi
    ;;

  *)
    echo "unknown platform: $platform" >&2
    exit 1
    ;;
esac

LC_ALL=C sort "$facts"
