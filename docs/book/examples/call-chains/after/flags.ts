// What the Vane service does for the SDKs and for the console.
import * as registry from "./registry.ts";
import * as audit from "./audit.ts";
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
