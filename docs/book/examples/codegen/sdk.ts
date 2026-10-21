// A piece of the backend SDK, written by hand on the generated client:
// it asks the service for the status of a flag at most once a second,
// and uses its cached answer in between, as the protocol asks of an SDK.
import { getStatus } from "./generated/node.ts";
import type { FlagStatus } from "./generated/types.ts";

const answers = new Map<string, { time: number; status: Promise<FlagStatus> }>();

export function flagStatus(flag: string, now: number = Date.now()): Promise<FlagStatus> {
  const answer = answers.get(flag);
  if (answer !== undefined && now - answer.time < 1000) return answer.status;
  const status = getStatus({ flag });
  answers.set(flag, { time: now, status });
  return status;
}
