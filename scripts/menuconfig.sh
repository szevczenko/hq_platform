#!/bin/bash
#
# Linux-only Kconfig menu script using Python kconfiglib.
# Installs kconfiglib via pipx (preferred) or a local venv, then runs menuconfig.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
KCONFIG_FILE="$PROJECT_DIR/Kconfig"
VENV_DIR="$PROJECT_DIR/.kconfig-venv"

if [ "$(uname -s)" != "Linux" ]; then
    echo "Error: scripts/menuconfig.sh is supported only on Linux."
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "Error: python3 not found."
    exit 1
fi

if [ ! -f "$KCONFIG_FILE" ]; then
    echo "Error: Kconfig file not found at $KCONFIG_FILE"
    exit 1
fi

# Resolve which Python can import kconfiglib / run menuconfig ----------------

# Option 1: already on PATH (e.g. user installed it globally or via pipx)
if python3 -c "import kconfiglib" >/dev/null 2>&1; then
    MENUCONFIG_CMD="python3 -m menuconfig"

# Option 2: use pipx (recommended on modern Debian/Ubuntu)
elif command -v pipx >/dev/null 2>&1; then
    echo "Installing kconfiglib via pipx..."
    pipx install kconfiglib
    # pipx installs the 'menuconfig' script; find it
    MENUCONFIG_CMD="$(pipx environment --value PIPX_BIN_DIR)/menuconfig"

# Option 3: create a local venv (no pipx, externally-managed pip)
else
    echo "pipx not found. Creating local venv at $VENV_DIR ..."
    if ! python3 -m venv "$VENV_DIR" >/dev/null 2>&1; then
        echo "Error: python3-venv not available."
        echo "Install it with: sudo apt install python3-venv"
        echo "Or install pipx with: sudo apt install pipx"
        exit 1
    fi
    "$VENV_DIR/bin/pip" install --quiet kconfiglib
    MENUCONFIG_CMD="$VENV_DIR/bin/menuconfig"
fi

# Run menuconfig -------------------------------------------------------------

echo "Launching menuconfig..."
cd "$PROJECT_DIR"

if $MENUCONFIG_CMD "$KCONFIG_FILE"; then
    echo
    echo "menuconfig finished."
    echo "Configuration saved to: ${KCONFIG_CONFIG:-.config}"
    echo
    echo "To generate a minimal defconfig run:"
    echo "  python3 -m savedefconfig Kconfig    (if kconfiglib is in PATH)"
    echo "  $VENV_DIR/bin/savedefconfig Kconfig  (if using local venv)"
else
    echo
    echo "menuconfig failed."
    exit 1
fi