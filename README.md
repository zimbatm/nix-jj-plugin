# nix-jj-plugin

**An experiment. Do not depend on it.** It exists to find out what a
non-Git fetcher needs from Nix, and it is published so that others can
argue with the design. It is not a product, it has no releases, no
stability promise and no support. It builds against one unmerged Nix
branch and will break when that branch moves. If the idea holds up, the
right home for it is Nix itself, not this repository.

A Nix fetcher for [Jujutsu](https://jj-vcs.dev) workspaces, as a plugin.

Without it, `nix build .` in a jj workspace that has no `.git` copies the
whole directory into the store — `.jj/`, build output, everything — and the
flake gets no `rev`. With it, the store holds what jj tracks, and a clean
workspace is locked to a revision.

`nix build` here is a Nix that carries the plugin. A plugin and the Nix
that loads it must be the same build, so the flake hands out both as one
command:

```console
$ nix run github:zimbatm/nix-jj-plugin -- build .
```

It needs the unmerged [NixOS/nix#16507](https://github.com/NixOS/nix/pull/16507), which
lets a fetcher claim a local directory. Nix releases do not carry that yet.

**Evaluation writes to your repository.** Every jj command records the
working copy first, and this fetcher runs `jj`, so evaluating a flake
creates a working-copy commit. That is how jj sees edits at all, and it is
still a read that changes the repository. Nothing else here surprises
people; this does.

## What it does

| Workspace | Result |
| --- | --- |
| `@` is empty (nothing in progress) | Files of `@`, `rev` is the commit ID of `@-`, input is locked |
| `@` has a change in progress | Files of `@`, no `rev`, warns like a dirty Git tree |
| Second workspace (`jj workspace add`) | Same as above; this is the case that has no `.git` at all |
| Colocated (`.jj` and `.git`) | Git handles it, unchanged — schemes are asked in name order |

Files come from `jj file list`, so jj's own ignore rules decide what reaches
the store.

Explicit URLs work too: `jj+file:///path/to/workspace`.

## Build

```console
$ nix build .#nix-jj-plugin
$ nix --plugin-files ./result/lib/jj-plugin.so flake metadata /path/to/workspace
```

On NixOS, a system Nix loads it through its own setting — but only a Nix
built from the same branch will load it at all:

```nix
nix.extraOptions = ''
  plugin-files = ${inputs.nix-jj-plugin.packages.${pkgs.system}.nix-jj-plugin}/lib/jj-plugin.so
'';
```

To build against a Nix worktree instead of the pinned one:

```console
$ nix develop /path/to/nix -c bash -c '
    export PKG_CONFIG_PATH=/path/to/nix/build/meson-uninstalled:$PKG_CONFIG_PATH
    g++ -shared -fPIC -std=c++23 $(pkg-config --cflags nix-fetchers nix-store nix-util) \
      -o jj-plugin.so src/jj.cc'
```

## Test

```console
$ NIX=$(nix build --no-link --print-out-paths .#nix)/bin/nix \
    PLUGIN=$(nix build --no-link --print-out-paths .#nix-jj-plugin)/lib/jj-plugin.so \
    tests/run.sh
```

## Known limits

- **A plugin has no stable ABI.** It links against Nix's C++ internals, so it
  must be built against the exact Nix that loads it. This is a place to try
  the design, not to depend on.
- **Local workspaces only, and that is not a gap.** jj has no protocol of
  its own: every remote command is `jj git …`, so a remote jj repository is
  a Git repository. Change IDs ride along in a `change-id` commit header, so
  `git+ssh:` already fetches them. A `jj+ssh:` would move the same bytes
  through more code. What only jj knows is local: the change in progress in
  `@`, which files it tracks, and workspaces with no `.git` at all.
- **Evaluation writes to the repository**, as at the top of this file.
- **No `change_id`.** jj's stable change ID is what makes it jj, and a Nix
  `rev` must parse as a hash, so only the commit ID is exposed today.

## Prior art

[shlevy/nix-plugins](https://github.com/shlevy/nix-plugins) is the plugin
that defined the pattern. It pins one exact Nix and gains a support commit
per release; a mismatch shows up as an undefined symbol at `dlopen`, and it
does not load into Lix or Determinate Nix. Take the ABI warning above from
there, not from theory.

[numtide/go2nix-nix-plugin](https://github.com/numtide/go2nix-nix-plugin)
(archived) runs `go list` during evaluation to resolve a Go dependency
graph. Same shape as this: let the real tool answer, rather than
reimplement it inside Nix.
