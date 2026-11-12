#!/usr/bin/env bash
# Stage a self-contained release tree from a build directory.
#
#   make-release.sh <build-dir> <output-dir> [version]
#
# Result: <output-dir>/{bin/, etc/cell-runtime/, python/, VERSION}. bin/cellctl
# and bin/plc-sim are wrappers that run the bundled Python packages with the
# release's own venv if it has one, else $CELL_PYTHON, else python3.
set -euo pipefail
[[ $# -ge 2 ]] || { echo "usage: $0 <build-dir> <output-dir> [version]" >&2; exit 2; }
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$1" out="$2" version="${3:-$(head -n1 "$root/VERSION" | tr -d '[:space:]')}"

[[ -f "$root/python/cell_proto/cell_pb2.py" ]] || "$root/scripts/gen_proto.sh" >/dev/null
rm -rf "$out"
cmake --install "$build" --prefix "$out" >/dev/null
echo "$version" >"$out/VERSION"

mkdir -p "$out/python"
for pkg in cellctl plc_sim cell_proto; do
  cp -r "$root/python/$pkg" "$out/python/"
done
cp "$root/python/requirements.txt" "$out/python/"
find "$out/python" -type d -name __pycache__ -prune -exec rm -rf {} +

for entry in cellctl:cellctl plc-sim:plc_sim; do
  name="${entry%%:*}" module="${entry##*:}"
  cat >"$out/bin/$name" <<WRAPPER
#!/usr/bin/env bash
# Runs the $module package bundled with this release.
release="\$(cd "\$(dirname "\$(readlink -f "\${BASH_SOURCE[0]}")")/.." && pwd)"
python="\${CELL_PYTHON:-python3}"
[[ -x "\$release/venv/bin/python" ]] && python="\$release/venv/bin/python"
export PYTHONPATH="\$release/python\${PYTHONPATH:+:\$PYTHONPATH}"
export CELL_CONFIG_DIR="\${CELL_CONFIG_DIR:-\$release/etc/cell-runtime}"
exec "\$python" -m $module "\$@"
WRAPPER
  chmod +x "$out/bin/$name"
done
echo "staged release $version in $out" >&2
