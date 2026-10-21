// Generated from the API description by program.xq and print.xsl. Do not edit.
import { call, expect } from "../runtime.ts";
import type { FlagsRequest, FlagSet, StatusRequest, FlagStatus } from "./types.ts";
import { checkFlagsRequest, checkFlagSet, checkStatusRequest, checkFlagStatus } from "./types.ts";

/** Every flag with its rules, for an SDK that evaluates flags itself. */
export async function getFlags(input: FlagsRequest): Promise<FlagSet> {
  expect<FlagsRequest>(input, checkFlagsRequest);
  const output = await call("getFlags", "query", input);
  return expect<FlagSet>(output, checkFlagSet);
}

/** The state of one flag and its failed checks since the last rollout step. */
export async function getStatus(input: StatusRequest): Promise<FlagStatus> {
  expect<StatusRequest>(input, checkStatusRequest);
  const output = await call("getStatus", "query", input);
  return expect<FlagStatus>(output, checkFlagStatus);
}
