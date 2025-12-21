# HP P1102w Printer Diagnostic Tool

A GTKmm-based Linux GUI tool for diagnosing and managing an HP P1102w printer,
including queue inspection, job management, and connectivity checks.

This repository intentionally keeps the application as a **single, monolithic
C++ source file** to preserve known-good behavior.

The file `HP_P1102w_Printer_Diagnostic_Tool.cpp` is the **source of truth**.

---

## ✅ System Requirements

- Linux
- g++ (C++17 or newer)
- GTKmm 3.x
- pkg-config

### Debian / Ubuntu / Mint
```bash
sudo apt install g++ pkg-config libgtkmm-3.0-dev
```

### Arch / CachyOS / Manjaro
```bash
sudo pacman -S gcc pkgconf gtkmm3
```

---

## 📥 Getting the Source

```bash
git clone https://github.com/mjdeiter/printer_diagnostics.git
cd printer_diagnostics
```

---

## 🔨 Quick Compile (No CMake)

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

---

## 🧱 Rebuilding with CMake (Recommended)

This produces an installable binary and desktop integration.

### 1️⃣ Create project structure

```bash
mkdir -p build
```

### 2️⃣ Generate build files

```bash
cd build
cmake ..
```

### 3️⃣ Build

```bash
cmake --build .
```

### 4️⃣ Run

```bash
./HP_P1102w_Printer_Diagnostic_Tool
```

---

## 📦 Install System-Wide (Optional)

```bash
sudo cmake --install .
```

After installation, the program can be run as:

```bash
HP_P1102w_Printer_Diagnostic_Tool
```

and will appear in the desktop application menu.

---

## 🖱 Creating a Desktop Shortcut (Optional)

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

Right-click → **Allow Launching**, then double-click to run.

---

## 🔒 Design Notes (Important)

- This project intentionally avoids refactoring into multiple source files.
- The UI layout and behavior are preserved exactly as originally implemented.
- Structural refactors should only be attempted after validating behavior
  against the known-good binary.

If in doubt: **do not modify the source file** — rebuild it instead.

---

## 🧰 Troubleshooting

### GTKmm not found
```text
Package 'gtkmm-3.0' not found
```

Install the GTKmm development package for your distro (see above).

---

## 📜 License

Personal / internal tool.  
No warranty is provided.
