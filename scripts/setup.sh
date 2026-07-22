#!/usr/bin/env bash
# Setup script for bl-led on CachyOS / Arch Linux
# Run once to install nRF Connect SDK and dependencies.
# Requires: uv  (https://docs.astral.sh/uv/)
set -euo pipefail

NCS_DIR="${NCS_DIR:-$HOME/ncs}"
NCS_VERSION="${NCS_VERSION:-v2.7.0}"
VENV="$NCS_DIR/.venv"

echo "==> bl-led setup: nRF Connect SDK $NCS_VERSION → $NCS_DIR"
echo ""

# ── 1. System packages ────────────────────────────────────────────────────────
echo "[1/5] Installing system dependencies via pacman..."
sudo pacman -S --needed --noconfirm \
    python cmake ninja dtc git \
    arm-none-eabi-gcc arm-none-eabi-newlib

# ── 2. uv + west ─────────────────────────────────────────────────────────────
echo "[2/5] Installing west via uv..."

if ! command -v uv &>/dev/null; then
    echo "  uv not found — installing via the official installer..."
    curl -LsSf https://astral.sh/uv/install.sh | sh
    # The installer adds ~/.local/bin or ~/.cargo/bin — reload PATH
    export PATH="$HOME/.local/bin:$HOME/.cargo/bin:$PATH"
fi

uv --version

# Install west into an isolated venv inside the NCS workspace.
# All Zephyr Python requirements go into the same venv so west can find them.
mkdir -p "$NCS_DIR"
uv venv "$VENV"

# shellcheck source=/dev/null
source "$VENV/bin/activate"

uv pip install west
west --version

# ── 3. nRF Connect SDK ────────────────────────────────────────────────────────
if [ -d "$NCS_DIR/.west" ]; then
    echo "[3/5] nRF Connect SDK workspace already exists at $NCS_DIR — skipping init."
    echo "      To re-initialise, delete $NCS_DIR and re-run this script."
else
    echo "[3/5] Initialising nRF Connect SDK at $NCS_DIR..."
    echo "      This clones ~6 GB of source — can take 15–45 minutes."
    cd "$NCS_DIR"
    west init -m https://github.com/nrfconnect/sdk-nrf --mr "$NCS_VERSION"
    west update
    west zephyr-export
fi

# ── 4. Python requirements ────────────────────────────────────────────────────
echo "[4/5] Installing Python requirements into $VENV..."
cd "$NCS_DIR"
uv pip install -r zephyr/scripts/requirements.txt
uv pip install -r nrf/scripts/requirements.txt
uv pip install -r bootloader/mcuboot/scripts/requirements.txt

# ── 5. Done ───────────────────────────────────────────────────────────────────
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo ""
echo "[5/5] Setup complete!"
echo ""
echo "Before each build, activate the venv:"
echo ""
echo "  bash / zsh:  source $VENV/bin/activate"
echo "  fish:        source $VENV/bin/activate.fish"
echo ""
echo "Then build:"
echo ""
echo "  cd $PROJECT_DIR"
echo "  west build -b nice_nano"
echo ""
echo "Flash by double-tapping RESET on the nice!nano and copying:"
echo ""
echo "  cp build/zephyr/zephyr.uf2 /run/media/\$USER/NICENANO/"
echo ""
echo "TIP: add 'source $VENV/bin/activate.fish' to ~/.config/fish/config.fish"
echo "     to activate automatically in every new terminal."
