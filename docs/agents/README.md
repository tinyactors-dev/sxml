<!-- Generated from spec/articles by tools/spec/spec generate. Do not edit: change it with tools/spec/spec (spec/README.md). -->

# Working with agents

sxml is built by an orchestrator session that delegates packets to subagents.
This directory holds the prompts those agents get, so the work can be picked up
by any later session and is never stuck in one.

- `BRIEF.md`: the common brief every agent follows.
- `PACKET.md`: how to work a packet from `docs/tasks/`.
- `QUEUE.md`: what runs now, what comes next, and the leftovers that no packet
  owns yet. The orchestrator keeps it current.

## How a session continues the work

1. Read `CLAUDE.md`, then the queue (`tools/spec/spec markdown agents/QUEUE`)
   and the state of the packets (`tools/spec/spec status`).
2. Pick the next packets from the queue and `tools/spec/spec status PHASE`.
   Packets that own disjoint files may run at the same time.
3. Give each agent a prompt that says: read the brief and the packet guide
   (`tools/spec/spec markdown agents/BRIEF`, `tools/spec/spec markdown
   agents/PACKET`), the packet's number (`tools/spec/spec brief N.M`), the
   decisions to read first, the goal and its done-when, and which other packets
   run at the same time and the files they own.
4. When an agent reports, record what it left over in the queue
   (`tools/spec/spec text agents/QUEUE#SECTION`, edit, then `tools/spec/spec
   amend agents/QUEUE#SECTION --body=-`), and commit that change on its own.

The prompt of each packet is its article (`tools/spec/spec brief N.M`) plus the
lines given in step 3; anything an agent decides goes into a decision and its
commit messages, so the history explains itself.
