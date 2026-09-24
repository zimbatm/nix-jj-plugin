#!/usr/bin/env bash
# Usage: NIX=/path/to/nix PLUGIN=/path/to/jj-plugin.so tests/run.sh
set -euo pipefail

NIX=${NIX:-nix}
PLUGIN=${PLUGIN:?set PLUGIN to the built jj-plugin.so}

command -v jj > /dev/null || {
  echo "jj not on PATH"
  exit 1
}

root=$(mktemp -d)
trap 'chmod -R +w "$root" 2>/dev/null; rm -rf "$root"' EXIT

FLAKE='{ outputs = { self }: { files = builtins.attrNames (builtins.readDir self); srcRev = self.rev or "none"; }; }'

jjnix() { "$NIX" --plugin-files "$PLUGIN" "$@"; }

# A jj repo that is not colocated: the files are what jj tracks, and .jj is
# not one of them.
repo=$root/plain
mkdir -p "$repo"
jj git init --config git.colocate=false "$repo" > /dev/null 2>&1
echo "$FLAKE" > "$repo/flake.nix"
jj -R "$repo" describe -m init > /dev/null 2>&1
jj -R "$repo" new > /dev/null 2>&1

got=$(cd "$repo" && jjnix eval --json .#files)
[[ $got == '["flake.nix"]' ]] || {
  echo "plain: got $got"
  exit 1
}

# A clean working copy reports the revision of its parent.
rev=$(cd "$repo" && jjnix eval --raw .#srcRev)
[[ $rev == "$(jj -R "$repo" log --no-graph -r @- -T commit_id)" ]] || {
  echo "plain: rev $rev"
  exit 1
}

# A change in progress has no revision, and says so.
echo wip > "$repo/wip.txt"
out=$(cd "$repo" && jjnix eval --raw .#srcRev 2>&1)
[[ $out == *"change in progress"* && $out == *none* ]] || {
  echo "dirty: got $out"
  exit 1
}
out=$(cd "$repo" && jjnix eval --no-allow-dirty --raw .#srcRev 2>&1 || true)
[[ $out == *"change in progress"* ]] || {
  echo "dirty: not refused: $out"
  exit 1
}
jj -R "$repo" commit -m wip > /dev/null 2>&1

# A second workspace has no .git at all. Ignored build output stays out of
# the store; without the plugin the whole directory is copied instead.
ws=$root/ws
jj -R "$repo" workspace add "$ws" > /dev/null 2>&1
printf 'target/\n' > "$ws/.gitignore"
mkdir -p "$ws/target"
echo junk > "$ws/target/out.bin"

got=$(cd "$ws" && jjnix eval --json .#files)
[[ $got == '[".gitignore","flake.nix","wip.txt"]' ]] || {
  echo "workspace: got $got"
  exit 1
}

# A colocated repo keeps resolving as Git: schemes are asked in name order.
colo=$root/colo
mkdir -p "$colo"
jj git init --config git.colocate=true "$colo" > /dev/null 2>&1
echo "$FLAKE" > "$colo/flake.nix"
jj -R "$colo" describe -m init > /dev/null 2>&1
url=$(cd "$colo" && jjnix flake metadata --json | jq -r .resolvedUrl)
[[ $url == git+file://* ]] || {
  echo "colocated: got $url"
  exit 1
}

echo "all tests passed"
