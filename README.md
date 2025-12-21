from pathlib import Path

readme = """# HP P1102w Printer Diagnostic Tool

A GTKmm-based Linux GUI tool for diagnosing and managing an HP P1102w printer,
including queue inspection, job management, and connectivity checks.

This repository intentionally keeps the application as a single, monolithic
C++ source file to preserve known-good behavior.

The file `HP_P1102w_Printer_Diagnostic_Tool.cpp` is the source of truth.

---

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
