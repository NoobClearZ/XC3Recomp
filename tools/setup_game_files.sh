#!/usr/bin/env bash
# Point targets/ at the player's own extracted game files on Linux (equivalent to tools/setup_game_files.ps1).
#   tools/setup_game_files.sh [folder]        exit 0 = ready, 2 = cancelled
#   tools/setup_game_files.sh --dlc [folder]  exit 0 = DLC linked, 2 = cancelled
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
py="$root/.venv/bin/python"
[ -x "$py" ] || py="$(command -v python3)"

dlc=0
if [ "${1:-}" = "--dlc" ] || [ "${1:-}" = "-Dlc" ]; then
  dlc=1
  shift
fi
folder="${1:-}"

pick_folder() {
  local title="$1"
  if command -v zenity >/dev/null 2>&1; then
    zenity --file-selection --directory --title="$title" 2>/dev/null || return 2
  elif command -v kdialog >/dev/null 2>&1; then
    kdialog --getexistingdirectory "$HOME" --title "$title" 2>/dev/null || return 2
  else
    echo "No GUI folder picker (zenity/kdialog) found. Pass folder path as argument:" >&2
    echo "  $0 <folder>" >&2
    return 1
  fi
}

set_link() {
  local name="$1" target="$2"
  mkdir -p "$root/targets"
  local link="$root/targets/$name"
  if [ -L "$link" ]; then
    rm -f "$link"
  elif [ -e "$link" ]; then
    echo "targets/$name is a real folder (not a symlink). Move or rename it, then run this again."
    return 1
  fi
  ln -s "$target" "$link"
}

find_dlc() {
  find -L "$1" -maxdepth 4 -type f -name 'bf3_dlc*.arh' 2>/dev/null | head -n 1 | xargs -r dirname
}

if [ "$dlc" = 1 ]; then
  if [ -z "$folder" ]; then
    folder="$(pick_folder "Choose the folder with the DLC files (bf3_dlc01.arh, bf3_dlc01.ard, ...)")" || {
      rc=$?; [ "$rc" = 2 ] && echo "Cancelled."; exit "$rc"
    }
  fi
  [ -d "$folder" ] || { echo "No such folder: $folder"; exit 1; }
  folder="$(realpath "$folder")"
  d="$(find_dlc "$folder")"
  [ -n "$d" ] || { echo "No DLC files found in that folder."; exit 1; }
  echo "dlcs: $d"
  "$py" -X utf8 "$root/tools/extract/check_dump.py" --dlcs "$d"
  set_link dlcs "$d"
  echo "DLC ready: targets/dlcs points to it."
  exit 0
fi

if [ -z "$folder" ]; then
  folder="$(pick_folder "Choose the folder containing exefs and romfs (XC3 2.2.0 or 2.2.1)")" || {
    rc=$?; [ "$rc" = 2 ] && echo "Cancelled."; exit "$rc"
  }
fi
[ -d "$folder" ] || { echo "No such folder: $folder"; exit 1; }
folder="$(realpath "$folder")"

exefs="$(find -L "$folder" -maxdepth 4 -type d 2>/dev/null | while read -r d; do [ -f "$d/main" ] && [ -f "$d/main.npdm" ] && { echo "$d"; break; }; done)"
romfs="$(find -L "$folder" -maxdepth 4 -type d 2>/dev/null | while read -r d; do { [ -f "$d/bf3.arh" ] || [ -d "$d/bdat" ]; } && { echo "$d"; break; }; done)"

if [ -z "$exefs" ] || [ -z "$romfs" ]; then
  if find -L "$folder" -maxdepth 4 -type f \( -name '*.xci' -o -name '*.nsp' -o -name '*.nca' -o -name '*.nsz' -o -name '*.xcz' \) 2>/dev/null | grep -q .; then
    echo "Those are game packages, not game files."
    echo "XC3Recomp needs extracted exefs and romfs folders, not .xci / .nsp / .nca files."
  else
    echo "No game files found in that folder."
  fi
  exit 1
fi

echo "exefs: $exefs"
echo "romfs: $romfs"
set +e
"$py" -X utf8 "$root/tools/extract/check_dump.py" --exefs "$exefs" --romfs "$romfs"
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
  [ "$rc" = 4 ] && echo "XC3Recomp needs version 2.2.0 or 2.2.1: the base game with that update applied."
  [ "$rc" = 5 ] && echo "Some romfs files are missing or different (incomplete dump or missing 2.2.0/2.2.1 update)."
  exit 1
fi

set_link exefs "$exefs"
set_link romfs "$romfs"
echo "Game files ready: targets/ points to them."

d="$(find_dlc "$folder")"
if [ -n "$d" ] && "$py" -X utf8 "$root/tools/extract/check_dump.py" --dlcs "$d"; then
  set_link dlcs "$d" && echo "DLC ready: targets/dlcs -> $d"
fi
exit 0
