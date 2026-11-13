#!/usr/bin/env bash
# Install a staged release (see make-release.sh) under $CELL_PREFIX/releases/<version>.
#
#   install.sh <staged-release-dir> [--venv] [--activate] [--units]
#
#   --venv      create releases/<version>/venv and pip-install the Python dependencies
#   --activate  point `current` at this release (default when nothing is installed yet)
#   --units     install the systemd unit files and enable cell.target (needs root)
#
# Installing never touches the running cell; use upgrade.sh to switch to a release.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

[[ $# -ge 1 ]] || die "usage: $0 <staged-release-dir> [--venv] [--activate] [--units]"
stage="$1"; shift
venv=0 activate=0 units=0
for arg in "$@"; do
  case "$arg" in
    --venv) venv=1 ;;
    --activate) activate=1 ;;
    --units) units=1 ;;
    *) die "unknown option $arg" ;;
  esac
done

[[ -f "$stage/VERSION" && -d "$stage/bin" ]] || die "'$stage' is not a staged release"
version="$(head -n1 "$stage/VERSION" | tr -d '[:space:]')"
[[ "$version" =~ ^[A-Za-z0-9._+-]+$ ]] || die "invalid version '$version'"
dest="$(releases_dir)/$version"
[[ ! -e "$dest" ]] || die "release $version is already installed (releases are immutable)"

# Copy to a temporary name and rename, so a half-copied tree is never visible
# under its final name.
mkdir -p "$(releases_dir)"
tmp="$(releases_dir)/.$version.partial"
rm -rf "$tmp"
cp -a "$stage" "$tmp"
if (( venv )); then
  python3 -m venv "$tmp/venv"
  "$tmp/venv/bin/pip" install --quiet --disable-pip-version-check \
    $(grep -v '^pytest' "$tmp/python/requirements.txt")
fi
mv -T "$tmp" "$dest"
log "installed release $version -> $dest"

if (( units )); then
  unit_src="$(cd "$(dirname "${BASH_SOURCE[0]}")/../systemd" && pwd)"
  id -u cell >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin cell
  install -m 0644 "$unit_src"/*.service "$unit_src"/*.target /etc/systemd/system/
  systemctl daemon-reload
  systemctl enable cell.target "${CELL_UNITS[@]/%/.service}"
  log "installed and enabled systemd units"
fi

if (( activate )) || [[ -z "$(link_version current)" ]]; then
  switch_link current "$version"
  log "current -> $version"
fi
echo "$version"
