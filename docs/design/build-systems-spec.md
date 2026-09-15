# Build Systems Specification

This document is the authority on what HuxerUI's build systems must agree on.
It defines the default program model, the developer extension points, the
differences allowed between build systems, and the rules the SDK and the CLI
follow when a project may use more than one build system.
How each build system meets it belongs to [SDK, CLI, Platform Shell, and Library](sdk-cli.md)
for CMake and to [mcpp Build System](mcpp-build-system.md) for mcpp.

## Decisions

- A project built with its build system's defaults produces the same program as the same project built with another build system's defaults.
- A build system may offer capabilities another does not; such a capability is enabled explicitly and never by a default build.
- A developer extension point — platform code, platform metadata, dependencies, signing, a device — is supported by every build system; one not yet supported is recorded here with where it is tracked.
- A difference is allowed only when this document lists it with a reason.
- A change to the default program model changes this document, every build system and the conformance facts together.

## 1. Terms

- **Build system**: CMake with the platform shells it drives (Gradle, Xcode, WiX), or mcpp with the `mcpp:plugins` dist members.
- **Default build**: `huxerui create` followed by `huxerui build`, `huxerui run` or `huxerui package` with no option beyond the platform.
- **Program model**: what the user of the produced program can observe: the artifact, where it installs, what it links and loads, its resources, its identity, its permissions and its platform metadata.
- **Program model fact**: one observable property of an artifact, named `<platform>.<fact>` (§6).
- **Extension point**: a place a developer changes the program beyond the default: platform code, platform metadata, dependencies, signing, devices.
- **Capability**: a build-system-specific feature that changes the build or the program only when requested.

## 2. Default program model

Every build system must produce this for a default build.

| Aspect | Required |
|---|---|
| Framework linkage | Static into the application on Linux, Windows, macOS, iOS and the Web; a separate `libhuxerui.so` on Android. |
| Loading | On Android the launcher Activity loads the application library and `HuxerUIView` loads the framework library; a load failure is thrown as it is. |
| Resource package | One merged package per application: the framework's builtin resources, then each library's in dependency order, then the application's; a later package overrides an earlier one's variant of the same resource. |
| Resource namespaces | The application's is `app`. A library's is its `Package::Product` pair flattened to `package_product`, lower-cased, one segment when the two are equal (`HuxerUI::Live2D` is `huxerui_live2d`). |
| Resource location | `<executable>.resources/` on Linux and Windows, `Contents/Resources/HuxerUI` on macOS, the bundle root on iOS, `assets/` on Android, the preloaded file system on the Web. |
| Platform floors | Android API 23, iOS 15.0, macOS 12.0. |
| Profiling | Off in an SDK application. |
| Application identity | The bundle identifier, the Android application id and the Web page's storage key come from `--id`; the display name from the project name. |
| Icons | The SDK templates': the Linux SVG, the macOS `.icns`, the iOS icon set, the Android launcher set, and the Web favicon and touch icon. |
| Android manifest | The SDK application template's as the Android Gradle plugin packages it: no permission, its `<application>` and `<activity>` attributes, and `android:extractNativeLibs="false"`. |
| Android code | The launcher `<id>.MainActivity`, which loads the application's library through `BuildConfig`, and `BuildConfig` with the variant's fields. |
| Android native libraries | Stripped of their symbol tables and debug information, stored uncompressed and aligned to 16 KB pages. |
| Apple Info.plist | The SDK application template's entries: the display name on both platforms; on iOS a launch screen, the development region, the dictionary version and the supported orientations (iPhone: portrait and both landscapes; iPad: all four). |
| Windows | The GUI subsystem with a `main()` entry; the installer is a Burn Setup.exe chaining the MSI, whose interface is the application's own installer program, built for a package build only. |
| Package artifacts | An AppImage and a DMG named `<target>-<version>`, a Setup.exe named `<target>-Setup-<version>.exe`, one APK for the template's two ABIs, an iOS `.app` and a Web directory, published to `dist/<platform>/`. |
| Signing | Android: a release package is unsigned and a debug package is signed with the Android debug key; macOS: signed ad hoc; the iOS simulator and Windows: not signed. |
| Library template | Carries `resources/` with its strings. |
| Test suites | unit, runtime, ui and smoke. |

## 3. Developer extension points

Every build system must support these.
A row whose support is incomplete names what is missing and where it is tracked.

| Extension point | CMake | mcpp |
|---|---|---|
| Application Java | Gradle project | `android/java` |
| Application manifest and `res/` | Gradle project | `.android.manifest_template`, `android/res` |
| Resources from code (R classes), `BuildConfig` | Gradle | Generated when the package has code; `BuildConfig` always |
| Application Kotlin | Gradle Kotlin plugin | `android/kotlin`, with the framework's `android-kotlin` feature |
| Info.plist entries | Xcode project, `Info.plist.in` | `ios/Info.plist`, `macos/Info.plist` |
| Signing: identity, entitlements, keystore | Xcode and Gradle configuration | `.apple.identity`, `.apple.entitlements`, `.android.keystore` |
| A library's Android sources, `res/` and manifest | Gradle library module | The library's `android/{java,kotlin,res,assets}` and `android/AndroidManifest.xml`; direct dependencies only |
| JAR and AAR archives | Gradle | `android/libs/*.jar`, `android/libs/*.aar` |
| Maven dependencies | Gradle, resolved during the build | `.android.maven` with the framework's `android-maven` feature, resolved into `android/maven.lock` on request |
| iOS device | Xcode and `devicectl` | `huxerui run ios --device`, `.apple.provisioning_profile`; not measured on a device |
| A library's Web JavaScript | The library's CMake runs esbuild | The library's `build.mcpp` runs `xim:esbuild` |
| A library's Swift | Swift package | **Missing**: mcpp compiles no Swift (mcpp-community/mcpp#647, E2) |
| Transitive libraries' resources and platform sources | The CMake and Gradle graphs | **Missing**: a build program sees its direct dependencies only (mcpp-community/mcpp#647, E1) |

**Build hosts.** An Android application builds on the hosts Gradle supports under CMake. Under mcpp it builds on Linux only.
- The macOS host is blocked because mcpp links an Android row there with the Mach-O linker (mcpp-community/mcpp#647, E3).
- The Windows host is blocked because the NDK's Windows archive carries no libc++ module surface.

## 4. Allowed differences

A conformance fact whose values differ between build systems fails the check unless a line below names it.
Each line is a fact-name pattern, then the reason.
The conformance scripts read this block, so it is the only list.

<!-- allowed-differences:begin -->
```text
*.binary.*                                   toolchains differ; hashes, symbols and optimisation are not the program model
linux.runtime.gtk                            CMake links the distribution's GTK 4 and needs it on the host; mcpp links GTK from xlings payloads built against its own glibc, so the AppImage carries that closure (the host's GL, EGL and Vulkan serve both)
linux.desktop.X-AppImage-Version             dist-appimage records the package version for AppImage tools; CMake's desktop entry states none
macos.runtime.libcxx                         CMake links the system libc++; mcpp links a libc++ built for the 12.0 floor
ios.runtime.libcxx                           CMake links the system libc++; mcpp links the libc++ its objects match
ios.plist.launch_screen_form                 Xcode compiles the template's LaunchScreen.storyboard; mcpp states an empty UILaunchScreen, the same blank screen, because ibtool is not redistributable
ios.icon.container                           Xcode compiles an asset catalog; mcpp lists flat PNGs under CFBundleIcons, because actool is not redistributable
ios.plist.CFBundleVersion                    the Xcode template writes 1; mcpp takes [package] version
macos.plist.CFBundle*Version*                the macOS template writes none; mcpp takes [package] version
macos.plist.CFBundleExecutable               dist-apple names the executable; CMake's template leaves the system to find the one named after the bundle, the same file
macos.plist.NSHighResolutionCapable          dist-apple states it true, and a project's Info.plist can change the value but not remove the key; CMake's template states none, and whether a CMake bundle renders at full resolution without it is not measured
windows.*.msvcp140*.dll                      CMake links the MSVC runtime as DLLs and installs them beside each program; mcpp links it into the program, so there is none to install
windows.*.vcruntime140*.dll                  the same: the MSVC runtime is in the program under mcpp
android.version.*                            the Gradle template writes 1.0 and 1; mcpp takes [package] version, one source for every platform
android.artifact.name                        Gradle names the APK after its module and variant (app-release-unsigned.apk); dist-apk names it after the target
android.dex.class.org.huxerui.R              Gradle generates an R class for the framework's library module, which has no resources; nothing reads it
android.resources.file.mcpp-run.json         mcpp's `adb-run` reads the launcher from it; the program never reads it
web.page.file                                dist-web names the page index.html, which a static server serves for the directory; CMake names it after the target
```
<!-- allowed-differences:end -->

## 5. Build-system capabilities

A capability one build system has and another does not is allowed when it is enabled explicitly.
- A default build must not enable it.
- The conformance check does not cover it.
- Each build system documents its own list. For mcpp that is the guide's "Build and development enhancements" section, for example a shared framework on the desktop (`linkage = "shared"`) or Maven artifacts verified against a committed lock.

## 6. SDK and CLI rules

**SDK:**
- A change to sources or tools two build systems share must not change another build system's result.
- Every such change states its reason.
- Source parity (`huxerui-build-check`) must pass.

**CLI:**
- One interface for every build system; a verb dispatches to the project's build system.
- A command produces the same kind of artifact, in the same place, with the same exit semantics, under every build system.
- An option that does not apply is refused with the reason, never ignored.
- `doctor` is read-only.
- A build-system capability is used through that build system, not wrapped in the CLI.

## 7. Verification

- **Source parity:** `huxerui-build-check` asserts both build systems compile the same framework sources with the same definitions (mcpp Build workflow).
- **Default program model conformance:** `.github/workflows/build-systems-conformance.yml`.
  1. It creates the same application with each build system and runs `huxerui package <platform>` with defaults.
  2. `program_model_facts.sh` reads each artifact into sorted `<platform>.<fact>=<value>` lines.
  3. `compare_program_model.sh` fails on a difference §4 does not allow.
  4. Linux, Web and Android run on pull requests; every platform runs weekly and on request.
- **What the check does not cover:**
  - The two templates demonstrate different code, so the mcpp project takes the CMake project's `resources/`; the resource package then compares the build systems.
  - The CMake project builds the framework from the checkout (`--source`), which compiles profiling in as the repository's own build does, so profiling is not a fact.
  - An iOS device build is not packaged, because no runner has a device.

## 8. Changing the program model

A change to §2 is made in one change set that updates:
- this document;
- every build system;
- the conformance facts, or the allowed differences.
