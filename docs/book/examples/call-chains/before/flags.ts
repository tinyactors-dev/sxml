// What the flag service does for each request.
import * as registry from "./registry.ts";
import * as cache from "./cache.ts";
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
  const flags = await registry.getFlags();
  return flags.map(summarize);
}

function isOwner(flag: Flag, owner: string): boolean {
  return flag.owners.includes(owner);
}

export async function killFlag(key: string, owner: string): Promise<boolean> {
  const flag = (await cache.getFlag(key)) ?? (await registry.getFlag(key));
  if (flag.percentage === 0 || !isOwner(flag, owner)) {
    return false;
  }
  await registry.setPercentage(key, 0);
  return true;
}
