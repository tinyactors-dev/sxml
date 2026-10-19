# Working with agents

sxml is built by an orchestrator session that delegates packets to
subagents. This directory holds the prompts those agents get, so the work
can be picked up by any later session and is never stuck in one.

- `BRIEF.md`: the common brief every agent follows.
- `PACKET.md`: how to work a packet from `docs/tasks/`.
- `QUEUE.md`: what runs now, what comes next, and the leftovers that no
  packet owns yet. The orchestrator keeps it current.

## How a session continues the work

1. Read `CLAUDE.md`, then `QUEUE.md`.
2. Pick the next packets from `QUEUE.md` and the task files. Packets that
   own disjoint files may run at the same time.
3. Give each agent a prompt that says: read `docs/agents/BRIEF.md` and
   `docs/agents/PACKET.md`, the packet's number and task file, the
   decisions to read first, the goal and its done-when, and which other
   packets run at the same time and the files they own.
4. When an agent reports, record what it left over in `QUEUE.md`, and
   commit that change on its own.

The prompt of each packet is its entry in `docs/tasks/phase-N.md` plus
the lines given in step 3; anything an agent decides goes into
`docs/DECISIONS.md` and its commit messages, so the history explains itself.
