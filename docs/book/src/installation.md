# Installation

<!-- examples: getting-started -->

The first step is to get the `sxml` command. You can install a release
with mise, download a prebuilt archive, or build sxml from its source
code. All of them give you the same program: one file, `sxml`
(`sxml.exe` on Windows), with every feature of the library in it and no
dependencies.

## Installing with mise

[mise](https://mise.jdx.dev) installs development tools from their
GitHub releases. With it installed, one command picks the archive for
your system, unpacks it, and puts `sxml` on your `PATH`:

```sh
mise use -g github:tinyactors-dev/sxml
sxml --version
```

`-g` installs it for your user. Without it, mise records the tool in the
`mise.toml` of the current project, so everyone who works on that
project gets the same version; `github:tinyactors-dev/sxml@0.2.0` pins
one.

## Installing a release archive

Each release of sxml comes with an archive of the `sxml` command for
each supported platform, named `sxml-VERSION-TARGET`:

| Target | Archive | Program |
| --- | --- | --- |
| `x86_64-linux`, `aarch64-linux` | `.tar.gz` | `sxml`, a static executable that runs on any Linux distribution |
| `x86_64-macos`, `aarch64-macos` | `.tar.gz` | `sxml`, for macOS 11 and later |
| `x86_64-windows`, `aarch64-windows` | `.zip` | `sxml.exe`, for Windows 10 and later |
| `wasm32-wasi` | `.tar.gz` | `sxml.wasm`, for any WebAssembly runtime with WASI preview 1 |

Download the archive for your system from the releases page of the
repository, <https://github.com/tinyactors-dev/sxml/releases>, together with
the file `SHA256SUMS`. Check the archive, unpack it, and put the program
in a directory on your `PATH`. On Linux, for example:

```sh
sha256sum --check --ignore-missing SHA256SUMS
tar -xzf sxml-0.2.0-x86_64-linux.tar.gz
install -m 755 sxml-0.2.0-x86_64-linux/sxml ~/.local/bin/sxml
```

On macOS, a downloaded program that is not notarized is quarantined;
`xattr -d com.apple.quarantine sxml` lets it run. On Windows, unpack the
`.zip` and add its directory to the `Path` environment variable. The
WebAssembly program runs in a WASI runtime that gives it the directories
it may read, for example `wasmtime --dir=. sxml.wasm xpath '1 + 1'`.

Each archive also holds `USAGE.md`, the manual of the command.

## Building from source

To build sxml you need:

- [CMake](https://cmake.org/) 3.24 or later;
- [Ninja](https://ninja-build.org/);
- a C17 compiler: Clang or GCC on Linux and macOS, or MSVC on Windows.

sxml has no other dependencies: everything it uses is in its
repository. Get the source and build the release configuration:

```sh
git clone https://github.com/tinyactors-dev/sxml
cd sxml
cmake --preset release
cmake --build --preset release
```

The command is now `build/release/apps/sxml`. Put its directory on your
`PATH`:

```sh
export PATH="$PWD/build/release/apps:$PATH"
```

Add that line, with the full path, to your shell's startup file (such as
`~/.bashrc` or `~/.zshrc`) to keep it.

If you use [mise](https://mise.jdx.dev), the repository's `mise.toml`
fetches the exact tools it is built with, and one task builds sxml for
your machine and installs it into `~/.local/bin` (or the prefix you
name):

```sh
mise install
mise run install
mise run install -- /usr/local
```

## Checking the installation

To check that sxml is installed, ask it for its version:

```console
$ sxml --version
sxml 0.2.0
```

If you see a version number, you're all set. If your shell says it
cannot find `sxml`, check that the directory with the program in it is on
your `PATH`.

## Getting help

`sxml help` lists the commands, and `sxml help COMMAND` (or
`sxml COMMAND --help`) explains one of them, with examples:

```console
$ sxml help | head -5
sxml - check, validate, query and transform XML documents

Usage: sxml [--version] COMMAND [ARGUMENT...]

sxml checks XML 1.0 and 1.1 documents, validates them against DTDs
```

Appendix A, [The Command-Line Reference](appendix-cli.md), describes
every command and option.

## Following along

The book's examples are in the sxml repository, in
`docs/book/examples/`. If you built sxml from source, you already have
them. Each project of the book has a directory of its own, like a small
repository: the Vane specification is in `spec`, the diagrams in `svg`,
and so on. The chapters that use no project, such as this one, have a
directory of their own too. Each chapter says which directory it uses;
change into it, and every command will find its files. For this chapter
that is:

```sh
cd docs/book/examples/getting-started
```
