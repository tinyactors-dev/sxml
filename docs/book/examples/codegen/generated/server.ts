// Generated from the API description by program.xq and print.xsl. Do not edit.
import { serve } from "../runtime.ts";
import type { FlagsRequest, FlagSet, EvaluationContext, Evaluations, StatusRequest, FlagStatus, RolloutRequest, KillRequest, KillResult } from "./types.ts";
import { checkFlagsRequest, checkFlagSet, checkEvaluationContext, checkEvaluations, checkStatusRequest, checkFlagStatus, checkRolloutRequest, checkKillRequest, checkKillResult } from "./types.ts";

/** What the service does: one method for each remote function. */
export interface Implementation {
  /** Every flag with its rules, for an SDK that evaluates flags itself. */
  getFlags(input: FlagsRequest): Promise<FlagSet>;
  /** Whether each flag is on for one user, for an SDK in a browser page. */
  evaluateFlags(input: EvaluationContext): Promise<Evaluations>;
  /** The state of one flag and its failed checks since the last rollout step. */
  getStatus(input: StatusRequest): Promise<FlagStatus>;
  /** Rolls a flag out to a new percentage once its guardrail checks pass. */
  rollOutFlag(input: RolloutRequest): Promise<FlagStatus>;
  /** Turns a flag off for every user at once. */
  killFlag(input: KillRequest): Promise<KillResult>;
}

/** A Node.js request listener that checks every input and output. */
export function createHandler(implementation: Implementation) {
  return serve({
    getFlags: {
      kind: "query",
      checkInput: checkFlagsRequest,
      checkOutput: checkFlagSet,
      run: (input) => implementation.getFlags(input),
    },
    evaluateFlags: {
      kind: "query",
      checkInput: checkEvaluationContext,
      checkOutput: checkEvaluations,
      run: (input) => implementation.evaluateFlags(input),
    },
    getStatus: {
      kind: "query",
      checkInput: checkStatusRequest,
      checkOutput: checkFlagStatus,
      run: (input) => implementation.getStatus(input),
    },
    rollOutFlag: {
      kind: "command",
      checkInput: checkRolloutRequest,
      checkOutput: checkFlagStatus,
      run: (input) => implementation.rollOutFlag(input),
    },
    killFlag: {
      kind: "command",
      checkInput: checkKillRequest,
      checkOutput: checkKillResult,
      run: (input) => implementation.killFlag(input),
    },
  });
}
