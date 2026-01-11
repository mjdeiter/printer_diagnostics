# HP P1102w Printer Diagnostic Tool

A GTKmm-based Linux GUI tool for diagnosing and managing an HP P1102w printer, including queue inspection, job management, and connectivity checks.

<img width="1000" height="752" alt="image" src="https://github.com/user-attachments/assets/dbe9c6c8-3ae0-417e-a3d2-d9b4e70c9929" />

## Why This Exists

This tool was created to keep an older but well-loved HP P1102w printer usable on modern Linux systems and modern home networks.

While the printer itself is reliable, it can occasionally have trouble reconnecting or behaving correctly on newer network setups (for example, mesh routers such as Eero). When that happens, troubleshooting often requires bouncing between multiple command-line tools and utilities.

The goal of this program is to provide a single, straightforward GUI that covers the common bases for getting the printer back up and running quickly: inspecting the print queue, checking status, and performing basic connectivity and diagnostic checks in one place.

## Design Philosophy

This program intentionally remains a single, monolithic source file. The UI layout and behavior have been validated as known-good and are preserved exactly to avoid regressions introduced by refactoring.

Structural changes should only be made after validating behavior against the original, working binary.

**Stability, reproducibility, and ease of recovery take priority over architectural elegance in this project.**

## System Requirements

- Linux
- g++ (C++17 or newer)
- GTKmm 3.x
- pkg-config
- cmake
- git

### Debian / Ubuntu / Mint

```bash
sudo apt install g++ pkg-config cmake git libgtkmm-3.0-dev
```

### Arch / CachyOS / Manjaro

```bash
sudo pacman -S gcc pkgconf cmake git gtkmm3
```

## Getting the Source

```bash
git clone https://github.com/mjdeiter/printer_diagnostics.git
cd printer_diagnostics
```

## One-Command Rebuild (Recommended)

This repository includes a one-shot rebuild script that checks for required dependencies, configures CMake, builds the program, and optionally installs it system-wide — all without modifying the source file.

### Run the rebuild script

```bash
chmod +x rebuild_everything.sh
./rebuild_everything.sh
```

The script will:

- Verify required build tools
- Clone or update the repository
- Use `HP_P1102w_Printer_Diagnostic_Tool.cpp` as the source of truth
- Generate a safe CMake build
- Compile the application
- Optionally install it to `/usr/local/bin`

After installation, run the program with:

```bash
HP_P1102w_Printer_Diagnostic_Tool
```

## Manual Compile (No CMake)

If you just want to build and run the program directly:

```bash
g++ -std=c++17 HP_P1102w_Printer_Diagnostic_Tool.cpp \
  -o HP_P1102w_Printer_Diagnostic_Tool \
  `pkg-config --cflags --libs gtkmm-3.0`
```

Run it:

```bash
./HP_P1102w_Printer_Diagnostic_Tool
```

## Manual CMake Build (After Running the Rebuild Script)

This assumes `rebuild_everything.sh` has already been run at least once to generate `CMakeLists.txt` and the `src/` directory.

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Run locally:

```bash
./HP_P1102w_Printer_Diagnostic_Tool
```

Install system-wide (optional):

```bash
sudo cmake --install .
```

## Desktop Shortcut (Optional, Installed Version)

This shortcut assumes the program has been installed system-wide using `sudo cmake --install .`, so the binary is available in the PATH.

Create a launcher file:

```bash
nano ~/HP_P1102w_Printer_Diagnostic_Tool.desktop
```

Paste:

```ini
[Desktop Entry]
Type=Application
Name=HP P1102w Printer Diagnostic Tool
Comment=Printer diagnostics and queue management
Exec=HP_P1102w_Printer_Diagnostic_Tool
Icon=printer
Terminal=false
Categories=Utility;System;
```

Make it executable:

```bash
chmod +x ~/HP_P1102w_Printer_Diagnostic_Tool.desktop
```

Right-click the file and choose **Allow Launching**, then double-click to run.

## Design Notes

- This project intentionally avoids refactoring into multiple source files.
- UI layout and behavior are preserved exactly as originally implemented.
- Structural refactors should only be attempted after validating behavior against the known-good binary.

**If in doubt: do not modify the source file — rebuild it instead.**

## Troubleshooting

### GTKmm not found

```
Package 'gtkmm-3.0' not found
```

Install the GTKmm development package for your distro.

## License

Personal / internal tool. No warranty is provided.
