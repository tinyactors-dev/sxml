// The Vane service's remote functions, written by hand against the
// generated interface. The real service keeps its flags in PostgreSQL;
// this one keeps three in an array, so the example needs no database.
import type { Implementation } from "./generated/server.ts";
import type { FlagConfig, FlagStatus } from "./generated/types.ts";

interface Flag extends FlagConfig {
  owners: string[];
  failedChecks: number;
}

const flags: Flag[] = [
  {
    key: "new-checkout", state: "rolling-out", percentage: 20,
    rules: [{ attribute: "country", values: ["nl"], percentage: 100 }],
    owners: ["ada@example.com"], failedChecks: 0,
  },
  { key: "dark-mode", state: "stable", percentage: 100, rules: [], owners: ["grace@example.com"], failedChecks: 0 },
  { key: "beta-banner", state: "paused", percentage: 5, rules: [], owners: ["ada@example.com"], failedChecks: 3 },
];

function find(key: string): Flag {
  const flag = flags.find((flag) => flag.key === key);
  if (flag === undefined) throw new Error(`no flag has the key ${key}`);
  return flag;
}

function checkOwner(flag: Flag, owner: string) {
  if (!flag.owners.includes(owner)) throw new Error(`${owner} doesn't own ${flag.key}`);
}

/** A user's place from 0 to 99 for a flag: a hash of the flag and the user,
    so the user stays in the share while the percentage grows. */
function place(flag: string, user: string): number {
  let hash = 2166136261;
  for (const character of `${flag}/${user}`) {
    hash = Math.imul(hash ^ (character.codePointAt(0) ?? 0), 16777619) >>> 0;
  }
  return hash % 100;
}

function status(flag: Flag): FlagStatus {
  const { key, state, failedChecks } = flag;
  return state === "paused" ? { flag: key, state, failedChecks, retryAfter: 300 } : { flag: key, state, failedChecks };
}

export const vane: Implementation = {
  async getFlags() {
    // What an SDK needs to evaluate a flag: no owners, no counts.
    return { flags: flags.map(({ key, state, percentage, rules }) => ({ key, state, percentage, rules })) };
  },
  async evaluateFlags({ user, attributes }) {
    return {
      evaluations: flags.map((flag) => {
        const rule = flag.rules?.find((rule) =>
          attributes?.some(({ name, value }) => name === rule.attribute && rule.values?.includes(value)));
        return { flag: flag.key, on: place(flag.key, user) < (rule?.percentage ?? flag.percentage) };
      }),
    };
  },
  async getStatus({ flag }) {
    return status(find(flag));
  },
  async rollOutFlag({ flag: key, percentage, owner }) {
    const flag = find(key);
    checkOwner(flag, owner);
    if (flag.state === "paused") throw new Error(`${key} is paused`);
    Object.assign(flag, { state: "rolling-out", percentage, failedChecks: 0 });
    return status(flag);
  },
  async killFlag({ flag: key, owner }) {
    const flag = find(key);
    checkOwner(flag, owner);
    Object.assign(flag, { state: "stable", percentage: 0, rules: [] });
    return { flag: key, killed: true };
  },
};
