#!/usr/bin/env bash
#
# Assemble the distributable client into client_dist/ and generate its
# manifest.json (one {path, size, sha256} entry per file). The astonia3-updates
# nginx container serves client_dist/ as-is over HTTP; the standalone updater.exe
# diffs against manifest.json by SHA-256 and downloads only what changed.
#
# Re-run this after every client build to publish a new version.
#
# Source of truth = the from-source client tree (it carries our code changes and
# the authoritative assets):
#   bin/  moac.exe + DLLs   -- minus per-user state (data/, *.log, *.dat)
#   res/  gx*.zip, sx.zip, config/, cursor/, fonts, icon
#   eula.txt, license.txt   -- shipped at the install root
#
# Usage: ./pack_client.sh [VERSION]
#   VERSION defaults to CLIENT_VERSION from src/game/version.c.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT="$SCRIPT_DIR/../astonia_community_client"
DIST="$SCRIPT_DIR/client_dist"
FILES="$DIST/files"

if [ ! -d "$CLIENT/bin" ] || [ ! -d "$CLIENT/res" ]; then
    echo "ERROR: expected client tree at $CLIENT (with bin/ and res/)." >&2
    exit 1
fi

# Resolve the version: explicit arg wins, else parse version.c, else 0.0.0.
# (Portable sed — avoids grep -P, which some Git Bash locales reject.)
VERSION="${1:-}"
if [ -z "$VERSION" ]; then
    VERSION="$(sed -n 's/.*#[[:space:]]*define[[:space:]]\+CLIENT_VERSION[[:space:]]\+"\([0-9.]\+\)".*/\1/p' \
        "$CLIENT/src/game/version.c" 2>/dev/null | head -1 || true)"
fi
VERSION="${VERSION:-0.0.0}"

echo "Packing client version $VERSION"
echo "  from: $CLIENT"
echo "  into: $DIST"

# Fresh dist tree.
rm -rf "$FILES"
mkdir -p "$FILES"

# bin/: everything except per-user state (settings, map caches, chatlogs, logs).
mkdir -p "$FILES/bin"
( cd "$CLIENT/bin" && find . -type f \
    -not -path './data/*' \
    -not -name '*.log' \
    -not -name '*.dat' \
    -print0 ) | while IFS= read -r -d '' f; do
    mkdir -p "$FILES/bin/$(dirname "$f")"
    cp "$CLIENT/bin/$f" "$FILES/bin/$f"
done

# res/: all assets, structure preserved.
mkdir -p "$FILES/res"
cp -r "$CLIENT/res/." "$FILES/res/"

# Legal files at the install root (optional if missing).
for legal in eula.txt license.txt; do
    [ -f "$CLIENT/$legal" ] && cp "$CLIENT/$legal" "$FILES/$legal"
done

# Generate manifest.json. Paths are install-root-relative and forward-slashed;
# the updater builds each download URL as UPDATE_BASE_URL/files/<path>.
MANIFEST="$DIST/manifest.json"
{
    printf '{\n'
    printf '  "version": "%s",\n' "$VERSION"
    printf '  "files": [\n'
    first=1
    while IFS= read -r -d '' f; do
        rel="${f#"$FILES"/}"
        size="$(stat -c '%s' "$f")"
        sha="$(sha256sum "$f" | cut -d' ' -f1)"
        [ "$first" -eq 0 ] && printf ',\n'
        first=0
        printf '    { "path": "%s", "size": %s, "sha256": "%s" }' "$rel" "$size" "$sha"
    done < <(find "$FILES" -type f -print0 | sort -z)
    printf '\n  ]\n}\n'
} > "$MANIFEST"

count="$(grep -c '"path"' "$MANIFEST" || true)"
echo "Wrote $MANIFEST ($count files)"
echo "Dist size: $(du -sh "$FILES" | cut -f1)"
echo "Done. Start/refresh the host with:"
echo "  docker compose -f \"$SCRIPT_DIR/docker-compose.updates.yml\" up -d"
