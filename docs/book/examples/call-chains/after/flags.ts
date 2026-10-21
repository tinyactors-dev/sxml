// What the flag service does for each request.
import * as registry from "./registry.ts";
import * as audit from "./audit.ts";
import type { Flag } from "./http.ts";

export interface FlagSummary {
  key: string;
  name: string;
  state: Flag["state"];
}

function summarize(flag: Flag): FlagSummary {
  return { key: flag.key, name: flag.name, state: flag.state };
}

export async function listFlags(): Promise<FlagSummary[]> {
  const keys = await registry.getFlagKeys();
  const flags: Flag[] = [];
  for (const key of keys) {
    flags.push(await registry.getFlag(key));
  }
  return flags.map(summarize);
}

function isOwner(flag: Flag, owner: string): boolean {
  return flag.owners.includes(owner);
}

export async function killFlag(key: string, owner: string): Promise<boolean> {
  const flag = await registry.getFlag(key);
  if (flag.percentage === 0 || !isOwner(flag, owner)) {
    return false;
  }
  await registry.setPercentage(key, 0);
  await audit.record(key, "kill");
  return true;
}
