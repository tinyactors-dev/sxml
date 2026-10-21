// Generated from the API description by program.xq and print.xsl. Do not edit.
import { call, expect } from "../runtime.ts";
import type { RolloutRequest, FlagStatus, KillRequest, KillResult } from "./types.ts";
import { checkRolloutRequest, checkFlagStatus, checkKillRequest, checkKillResult } from "./types.ts";

/** Rolls a flag out to a new percentage once its guardrail checks pass. */
export async function rollOutFlag(input: RolloutRequest): Promise<FlagStatus> {
  expect<RolloutRequest>(input, checkRolloutRequest);
  const output = await call("rollOutFlag", "command", input);
  return expect<FlagStatus>(output, checkFlagStatus);
}

/** Turns a flag off for every user at once. */
export async function killFlag(input: KillRequest): Promise<KillResult> {
  expect<KillRequest>(input, checkKillRequest);
  const output = await call("killFlag", "command", input);
  return expect<KillResult>(output, checkKillResult);
}
