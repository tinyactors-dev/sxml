# Installation

<!-- examples: getting-started -->

The first step is to get the `sxml` command. You can download a
prebuilt archive, or build sxml from its source code. Both give you the
same program: one file, `sxml` (`sxml.exe` on Windows), with every
feature of the library in it and no dependencies.

## Installing a release archive

Each release of sxml comes with an archive of the `sxml` command for
each supported platform. Download the archive for your operating system
and processor from the releases page of the repository,
<https://github.com/dhamidi/sxml/releases>, unpack it, and put the
directory with `sxml` in it on your `PATH`.

On Linux or macOS, for example, if you unpacked the archive into
`~/opt/sxml`:

```sh
export PATH="$HOME/opt/sxml:$PATH"
```

Add that line to your shell's startup file (such as `~/.bashrc` or
`~/.zshrc`) to keep it. On Windows, add the directory to the `Path`
environment variable in the system settings.

> sxml has no tagged release yet. Until the first one is published,
> build it from source as the next section describes; it takes a few
> minutes.

## Building from source

To build sxml you need:

- [CMake](https://cmake.org/) 3.24 or later;
- [Ninja](https://ninja-build.org/);
- a C17 compiler: Clang or GCC on Linux and macOS, or MSVC on Windows.

sxml has no other dependencies: everything it uses is in its
repository. Get the source and build the release configuration:

```sh
git clone https://github.com/dhamidi/sxml
cd sxml
cmake --preset release
cmake --build --preset release
```

The command is now `build/release/apps/sxml`. Put its directory on your
`PATH`:

```sh
export PATH="$PWD/build/release/apps:$PATH"
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
`docs/book/examples/`, one directory for each chapter. If you built
sxml from source, you already have them. Change into the directory of
the chapter you are reading, and every command will find its files:

```sh
cd docs/book/examples/getting-started
```
