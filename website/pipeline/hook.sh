#!/bin/sh
# Runs after every edit the agent makes. In Claude Code,
# a PostToolUse hook that exits with status 2 shows the
# agent what it wrote to stderr.
sxml xquery rules.xq -o rules.xml --force &&
  sxml validate --schema=rules.xsd rules.xml &&
  sxml xquery --allow-file-access check.xq \
    -o violations.xml --force || exit 1
feedback=$(sxml transform feedback.xsl violations.xml)
[ -z "$feedback" ] && exit 0
echo "$feedback" >&2
exit 2
