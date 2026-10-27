Midnight Club: Los Angeles VR (Windows)
=======================================

A static recompilation of Midnight Club: Los Angeles Complete Edition (Xbox 360) for PC VR.
This release contains NO game files, code or keys. You need your own disc image, and the
game is built on your PC from it.

What you need
- Your own disc image: Midnight Club: Los Angeles Complete Edition, Xbox 360, USA/Europe
  (title 545407F8), as an .iso file.
- Windows 10 or 11 (64-bit), a Vulkan GPU, a PC VR headset with an OpenXR runtime
  (SteamVR, for example). DLSS needs an NVIDIA RTX GPU; without it the game uses its own
  anti-aliasing.
- About 15 GB of free disk space and an internet connection for the first run.

Play
1. Unzip this folder anywhere (a path without special characters is safest) and keep it together.
2. Start SteamVR (or your OpenXR runtime).
3. Double-click "Midnight Club LA VR.exe".
   First run: it asks for your disc image, then builds the game on this PC (10 to 30 minutes).
   If Microsoft's C++ Build Tools are not installed, it installs them from Microsoft first
   (free, several GB; Windows asks for administrator rights once).
   After that, the same double-click starts the game straight away.

In the game
- The pause menu has a VR page: render size (with the video memory it needs), headset size
  (no AA, MSAA, DLAA or a DLSS upscale), and a frame rate cap.
- Start (from the main menu) and Back (in the pause menu) recentre the view; so does F8.
- Options can be added after the program name, for example a shortcut to
  "Midnight Club LA VR.exe" --vr=false   (plays on the monitor)
  "Midnight Club LA VR.exe" --setup      (runs the first-run setup again, e.g. another image)

Files
- bin\        the game built on your PC (mcla.exe) and its libraries
- game\       the files extracted from your disc image
- setup\      the setup script, the project's source, the ReXGlue SDK and the build tools
- Logs of the first run: setup\logs. The game's own log: bin\mcla.log (when enabled).

What you build is for you, not for sharing.

Licences: the ReXGlue SDK (BSD-3) and its libraries, LLVM (Apache-2.0 with LLVM exception),
CMake (BSD-3), Ninja (Apache-2.0), Python (PSF), the OpenXR loader (Apache-2.0), the DLSS bridge
(MIT) and NVIDIA DLSS (NVIDIA's licence): see setup\licenses and setup\runtime\licenses.
Midnight Club is a trademark of its owners; this project is not affiliated with or endorsed by them.
