// What the Vane service does for the SDKs and for the console.
import * as registry from "./registry.ts";
import * as cache from "./cache.ts";
import type { Flag } from "./db.ts";

// What an SDK needs to evaluate a flag: no name, no owners.
export interface FlagSummary {
  key: string;
  state: Flag["state"];
  percentage: number;
  rules: Flag["rules"];
}

function summarize({ key, state, percentage, rules }: Flag): FlagSummary {
  return { key, state, percentage, rules };
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
