#!/usr/bin/env bash
set -euo pipefail

REPO_URL="https://github.com/mjdeiter/printer_diagnostics.git"
APP_NAME="HP_P1102w_Printer_Diagnostic_Tool"
SRC_FILE="HP_P1102w_Printer_Diagnostic_Tool.cpp"

echo "🔧 HP P1102w Printer Diagnostic Tool – Full Rebuild Script"
echo

# ------------------------------------------------------------
# 1. Ensure dependencies
# ------------------------------------------------------------
echo "🔍 Checking dependencies..."

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "❌ Missing dependency: $1"
    return 1
  }
}

missing=0
need_cmd g++ || missing=1
need_cmd pkg-config || missing=1
need_cmd cmake || missing=1

if [[ $missing -eq 1 ]]; then
  echo
  echo "⚠️  One or more build tools are missing."
  echo "Install GTKmm + build tools for your distro:"
  echo
  echo "Debian/Ubuntu:"
  echo "  sudo apt install g++ pkg-config cmake libgtkmm-3.0-dev"
  echo
  echo "Arch/CachyOS/Manjaro:"
  echo "  sudo pacman -S gcc pkgconf cmake gtkmm3"
  exit 1
fi

# ------------------------------------------------------------
# 2. Clone or update repo
# ------------------------------------------------------------
if [[ ! -d printer_diagnostics ]]; then
  echo "📥 Cloning repository..."
  git clone "$REPO_URL"
else
  echo "🔄 Updating repository..."
  cd printer_diagnostics
  git pull --ff-only
  cd ..
fi

cd printer_diagnostics

# ------------------------------------------------------------
# 3. Sanity check source of truth
# ------------------------------------------------------------
if [[ ! -f "$SRC_FILE" ]]; then
  echo "❌ ERROR: $SRC_FILE not found."
  exit 1
fi

echo "✅ Source of truth found: $SRC_FILE"

# ------------------------------------------------------------
# 4. Create CMake project (minimal, safe)
# ------------------------------------------------------------
echo "🧱 Preparing CMake build..."

mkdir -p src
cp -u "$SRC_FILE" "src/$SRC_FILE"

cat > CMakeLists.txt <<EOF
cmake_minimum_required(VERSION 3.16)
project(${APP_NAME} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED gtkmm-3.0)

add_executable(${APP_NAME}
    src/${SRC_FILE}
)

target_include_directories(${APP_NAME} PRIVATE \${GTKMM_INCLUDE_DIRS})
target_compile_options(${APP_NAME} PRIVATE \${GTKMM_CFLAGS_OTHER})
target_link_libraries(${APP_NAME} PRIVATE \${GTKMM_LIBRARIES})

install(TARGETS ${APP_NAME} RUNTIME DESTINATION bin)
EOF

# ------------------------------------------------------------
# 5. Build
# ------------------------------------------------------------
echo "🔨 Building..."

rm -rf build
mkdir build
cd build
cmake ..
cmake --build .

echo
echo "✅ Build complete."

# ------------------------------------------------------------
# 6. Ask about install
# ------------------------------------------------------------
echo
read -rp "📦 Install system-wide? (y/N): " install_ans

if [[ "$install_ans" =~ ^[Yy]$ ]]; then
  sudo cmake --install .
  echo "✅ Installed. Run with:"
  echo "  ${APP_NAME}"
else
  echo "ℹ️  Skipping install."
  echo "Run locally with:"
  echo "  ./build/${APP_NAME}"
fi

echo
echo "🎉 Done."
