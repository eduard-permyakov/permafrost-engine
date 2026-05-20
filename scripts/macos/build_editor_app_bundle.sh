#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APP_NAME="${PF_EDITOR_APP_NAME:-Permafrost Editor}"
BUNDLE_DIR="${PF_EDITOR_APP_BUNDLE_DIR:-$ROOT/dist/$APP_NAME.app}"
BACKEND="${RENDER_BACKEND:-METAL}"
SKIP_BUILD=0
LAUNCH=0
VERIFY=0

usage() {
    cat <<'EOF'
Usage: scripts/macos/build_editor_app_bundle.sh [options]

Options:
  --backend METAL|OPENGL     Build backend to package (default: METAL)
  --bundle-dir PATH          Output .app bundle path (default: dist/Permafrost Editor.app)
  --skip-build               Reuse the existing bin/pf-arm64
  --launch                   Open the app and leave it running
  --verify                   Open the app, verify the editor process, then stop it
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --backend)
            BACKEND="${2:?missing backend}"
            shift 2
            ;;
        --bundle-dir)
            BUNDLE_DIR="${2:?missing bundle dir}"
            shift 2
            ;;
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        --launch)
            LAUNCH=1
            shift
            ;;
        --verify)
            VERIFY=1
            LAUNCH=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

case "$BACKEND" in
    METAL|OPENGL) ;;
    *)
        echo "Unsupported backend: $BACKEND" >&2
        exit 2
        ;;
esac

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    make -C "$ROOT" pf PLAT=MACOS_ARM64 MACOS_ARM64_BUILD_READY=1 RENDER_BACKEND="$BACKEND"
fi

if [[ ! -x "$ROOT/bin/pf-arm64" ]]; then
    echo "Missing executable: $ROOT/bin/pf-arm64" >&2
    exit 1
fi

MACOS_DIR="$BUNDLE_DIR/Contents/MacOS"
RESOURCES_DIR="$BUNDLE_DIR/Contents/Resources"
RUNTIME_DIR="$RESOURCES_DIR/permafrost"
rm -rf "$BUNDLE_DIR"
mkdir -p "$MACOS_DIR" "$RUNTIME_DIR"

cat > "$BUNDLE_DIR/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleDisplayName</key>
    <string>$APP_NAME</string>
    <key>CFBundleExecutable</key>
    <string>permafrost-editor</string>
    <key>CFBundleIdentifier</key>
    <string>org.permafrostengine.editor.dev</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>$APP_NAME</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>0.1</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF

cat > "$BUNDLE_DIR/Contents/PkgInfo" <<'EOF'
APPL????
EOF

cp "$ROOT/bin/pf-arm64" "$MACOS_DIR/pf-arm64"
/usr/bin/ditto "$ROOT/assets" "$RUNTIME_DIR/assets"
/usr/bin/ditto "$ROOT/scripts" "$RUNTIME_DIR/scripts"
/usr/bin/ditto "$ROOT/shaders" "$RUNTIME_DIR/shaders"

cat > "$MACOS_DIR/permafrost-editor" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNTIME_DIR="$(cd "$SCRIPT_DIR/../Resources/permafrost" && pwd)"
cd "$RUNTIME_DIR"
LOG_PATH="${PF_EDITOR_APP_LOG:-/tmp/permafrost-editor.log}"
mkdir -p "$(dirname "$LOG_PATH")"
{
    echo "Permafrost Editor launch $(date)"
    echo "runtime=$RUNTIME_DIR"
} >> "$LOG_PATH"
exec "$SCRIPT_DIR/pf-arm64" ./ ./scripts/macos/pf_editor_app.py >> "$LOG_PATH" 2>&1
EOF
chmod +x "$MACOS_DIR/permafrost-editor"

cat > "$RESOURCES_DIR/README.txt" <<EOF
Development app bundle for the Permafrost Engine editor.

This bundle stages pf-arm64 plus the assets, scripts, and shaders required by
the editor under Contents/Resources/permafrost so macOS privacy controls do not
block the app from reading the repository checkout on Desktop/Documents.
EOF

echo "EDITOR_APP_BUNDLE_READY path=$BUNDLE_DIR backend=$BACKEND"

if [[ "$LAUNCH" -eq 1 ]]; then
    /usr/bin/open -n "$BUNDLE_DIR"
fi

if [[ "$VERIFY" -eq 1 ]]; then
    MATCH="pf-arm64 .*scripts/macos/pf_editor_app.py"
    pid=""
    for _ in {1..80}; do
        pid="$(pgrep -f "$MATCH" | head -n 1 || true)"
        if [[ -n "$pid" ]]; then
            break
        fi
        sleep 0.25
    done
    if [[ -z "$pid" ]]; then
        echo "EDITOR_APP_LAUNCH_FAIL no editor process found" >&2
        exit 1
    fi
    echo "EDITOR_APP_LAUNCH_READY pid=$pid"
    pkill -9 -f "$MATCH" || true
fi
