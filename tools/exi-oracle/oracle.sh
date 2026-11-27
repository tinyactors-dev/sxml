#!/bin/bash
# Runs the EXI oracle (README.md): builds it on first use (Maven fetches
# EXIficient 1.0.4 into ~/.m2), then `java Oracle "$@"`.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
build=${SXML_EXI_ORACLE_BUILD:-$here/build}
if [ ! -f "$build/classes/Oracle.class" ] || [ "$here/Oracle.java" -nt "$build/classes/Oracle.class" ]; then
    mkdir -p "$build/classes"
    mvn -q -f "$here/pom.xml" dependency:copy-dependencies -DoutputDirectory="$build/lib" >&2
    javac -nowarn -d "$build/classes" -cp "$build/lib/*" "$here/Oracle.java" 2>&1 | grep -v '^Note:\|^Picked up' >&2 || true
fi
exec java -Xss64m -cp "$build/classes:$build/lib/*" Oracle "$@" 2> >(grep -v '^Picked up' >&2)
