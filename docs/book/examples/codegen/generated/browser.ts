// Generated from the API description by program.xq and print.xsl. Do not edit.
import { call, expect } from "../runtime.ts";
import type { EvaluationContext, Evaluations, StatusRequest, FlagStatus } from "./types.ts";
import { checkEvaluationContext, checkEvaluations, checkStatusRequest, checkFlagStatus } from "./types.ts";

/** Whether each flag is on for one user, for an SDK in a browser page. */
export async function evaluateFlags(input: EvaluationContext): Promise<Evaluations> {
  expect<EvaluationContext>(input, checkEvaluationContext);
  const output = await call("evaluateFlags", "query", input);
  return expect<Evaluations>(output, checkEvaluations);
}

/** The state of one flag and its failed checks since the last rollout step. */
export async function getStatus(input: StatusRequest): Promise<FlagStatus> {
  expect<StatusRequest>(input, checkStatusRequest);
  const output = await call("getStatus", "query", input);
  return expect<FlagStatus>(output, checkFlagStatus);
}
