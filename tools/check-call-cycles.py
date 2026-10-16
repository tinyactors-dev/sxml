#!/usr/bin/env python3
"""Fails when project code has a cycle in its call graph (D71: no C recursion).

clang-tidy's misc-no-recursion sees recursion within one file only. This gate
compiles every project file of src/ and apps/ (not third_party/, not the
generated grammars/) to LLVM bitcode with the flags of compile_commands.json,
links them into one module and lists the strongly connected components of the
call graph (`opt -passes=print-callgraph-sccs`), so it also finds recursion
across files. Calls through function pointers are not in the call graph:
review still checks those (D71).

A cycle whose functions are all listed in tools/call-cycles-allowlist.txt is
reported but allowed; the list is temporary (D71) and must shrink to empty.
An allowlist entry that is in no cycle any more is an error too, so the list
is emptied as the code is fixed.

Usage: tools/check-call-cycles.py [BUILD-DIRECTORY]   (default build/debug)
Needs: python3, clang, llvm-link and opt (Debian/Ubuntu package `llvm`).
"""
import concurrent.futures
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

REPOSITORY_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALLOWLIST_PATH = os.path.join(REPOSITORY_ROOT, "tools", "call-cycles-allowlist.txt")
PROJECT_DIRECTORIES = ("src", "apps")


def find_tool(name):
    for candidate in (name, name + "-18", name + "-19", name + "-20", name + "-17"):
        path = shutil.which(candidate)
        if path:
            return path
    sys.exit(f"check-call-cycles: {name} not found (install the llvm package)")


def is_project_file(path):
    relative = os.path.relpath(path, REPOSITORY_ROOT)
    return relative.split(os.sep)[0] in PROJECT_DIRECTORIES and relative.endswith(".c")


def bitcode_command(entry, clang, output):
    arguments = entry.get("arguments") or shlex.split(entry["command"])
    command = [clang]
    skip_next = False
    for argument in arguments[1:]:
        if skip_next:
            skip_next = False
            continue
        if argument in ("-o", "-c"):
            skip_next = argument == "-o"
            continue
        if argument.startswith("-fsanitize") or argument == entry["file"]:
            continue
        command.append(argument)
    return command + ["-c", "-emit-llvm", "-O0", "-w", "-o", output, entry["file"]]


def compile_all(build_directory, clang):
    with open(os.path.join(build_directory, "compile_commands.json")) as handle:
        entries = json.load(handle)
    output_directory = os.path.join(build_directory, "call-cycles")
    os.makedirs(output_directory, exist_ok=True)
    jobs = {}
    for entry in entries:
        path = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
        if not is_project_file(path) or path in jobs:
            continue
        relative = os.path.relpath(path, REPOSITORY_ROOT)
        output = os.path.join(output_directory, relative.replace(os.sep, "__") + ".bc")
        jobs[path] = (entry, output)

    def run(job):
        entry, output = job
        result = subprocess.run(
            bitcode_command(entry, clang, output),
            cwd=entry["directory"], capture_output=True, text=True)
        if result.returncode != 0:
            sys.exit(f"check-call-cycles: cannot compile {entry['file']}:\n{result.stderr}")
        return output

    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        outputs = list(pool.map(run, jobs.values()))
    return {output: os.path.relpath(path, REPOSITORY_ROOT)
            for path, (_, output) in jobs.items()}, output_directory


def plain_name(name):
    # llvm-link renames colliding static functions to name.N.
    return re.sub(r"\.\d+$", "", name)


def defining_files(bitcode_files, llvm_nm):
    files_of = {}
    for bitcode, source in bitcode_files.items():
        result = subprocess.run([llvm_nm, "--defined-only", bitcode],
                                capture_output=True, text=True, check=True)
        for line in result.stdout.splitlines():
            fields = line.split()
            if len(fields) >= 2 and fields[-2] in ("T", "t"):
                files_of.setdefault(fields[-1], set()).add(source)
    return files_of


def find_cycles(bitcode_files, output_directory):
    llvm_link = find_tool("llvm-link")
    opt = find_tool("opt")
    linked = os.path.join(output_directory, "all.bc")
    subprocess.run([llvm_link, "-o", linked, *sorted(bitcode_files)], check=True)
    result = subprocess.run(
        [opt, "-passes=print-callgraph-sccs", "-disable-output", linked],
        capture_output=True, text=True, check=True)
    cycles = []
    for line in (result.stdout + result.stderr).splitlines():
        match = re.match(r"SCC #\d+: (.*?)( \(Has self-loop\))?\.?$", line)
        if not match:
            continue
        names = [name.strip() for name in match.group(1).split(",")]
        if len(names) > 1 or match.group(2):
            cycles.append(sorted({plain_name(name) for name in names}))
    return cycles


def read_allowlist():
    allowed = set()
    with open(ALLOWLIST_PATH) as handle:
        for line in handle:
            line = line.split("#", 1)[0].strip()
            if line:
                allowed.add(line)
    return allowed


def main():
    build_directory = os.path.abspath(
        sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPOSITORY_ROOT, "build", "debug"))
    clang = find_tool("clang")
    bitcode_files, output_directory = compile_all(build_directory, clang)
    cycles = find_cycles(bitcode_files, output_directory)
    files_of = defining_files(bitcode_files, find_tool("llvm-nm"))
    allowed = read_allowlist()

    failed = False
    in_cycles = set()
    for cycle in cycles:
        in_cycles.update(cycle)
        is_allowed = all(name in allowed for name in cycle)
        sources = sorted({source for name in cycle for source in files_of.get(name, ())})
        label = "allowed (temporary, D71)" if is_allowed else "error"
        print(f"check-call-cycles: {label}: call cycle among these functions of {', '.join(sources)}: "
              f"{', '.join(cycle)}")
        failed = failed or not is_allowed
    for name in sorted(allowed - in_cycles):
        print(f"check-call-cycles: error: {name} is in no call cycle any more; "
              f"remove it from tools/call-cycles-allowlist.txt")
        failed = True
    if failed:
        print("check-call-cycles: project code must not recurse (D71)")
        return 1
    print(f"check-call-cycles: {len(bitcode_files)} files, "
          f"{len(cycles)} allowed cycles")
    return 0


if __name__ == "__main__":
    sys.exit(main())
