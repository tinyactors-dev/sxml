// Generated from the XSD schema by program.xq and print.xsl. Do not edit.
import { characterCount, checkBoolean, checkInteger, checkObject, checkString } from "../runtime.ts";

export type FlagKey = string;

export function checkFlagKey(value: unknown, path: string, problems: string[]): value is FlagKey {
  if (!checkString(value, path, problems)) return false;
  const count = problems.length;
  if (!/^(?:[a-z0-9][a-z0-9-]*)$/u.test(value)) problems.push(path + ": must match [a-z0-9][a-z0-9-]*");
  if (characterCount(value) > 64) problems.push(path + ": must have at most 64 characters");
  return problems.length === count;
}

export type Owner = string;

export function checkOwner(value: unknown, path: string, problems: string[]): value is Owner {
  if (!checkString(value, path, problems)) return false;
  const count = problems.length;
  if (!/^(?:[^@ ]+@[^@ ]+)$/u.test(value)) problems.push(path + ": must match [^@ ]+@[^@ ]+");
  return problems.length === count;
}

export type Environment = "production" | "staging";

export function checkEnvironment(value: unknown, path: string, problems: string[]): value is Environment {
  if (!checkString(value, path, problems)) return false;
  const count = problems.length;
  if (!["production", "staging"].includes(value)) problems.push(path + ": must be one of production, staging");
  return problems.length === count;
}

export type State = "stable" | "rolling-out" | "paused";

export function checkState(value: unknown, path: string, problems: string[]): value is State {
  if (!checkString(value, path, problems)) return false;
  const count = problems.length;
  if (!["stable", "rolling-out", "paused"].includes(value)) problems.push(path + ": must be one of stable, rolling-out, paused");
  return problems.length === count;
}

export type Percentage = number;

export function checkPercentage(value: unknown, path: string, problems: string[]): value is Percentage {
  if (!checkInteger(value, path, problems)) return false;
  const count = problems.length;
  if (value < 0) problems.push(path + ": must be at least 0");
  if (value > 100) problems.push(path + ": must be at most 100");
  return problems.length === count;
}

export type FailureCount = number;

export function checkFailureCount(value: unknown, path: string, problems: string[]): value is FailureCount {
  if (!checkInteger(value, path, problems)) return false;
  const count = problems.length;
  if (value < 0) problems.push(path + ": must be at least 0");
  if (value > 10) problems.push(path + ": must be at most 10");
  return problems.length === count;
}

export type Seconds = number;

export function checkSeconds(value: unknown, path: string, problems: string[]): value is Seconds {
  if (!checkInteger(value, path, problems)) return false;
  const count = problems.length;
  if (value < 1) problems.push(path + ": must be at least 1");
  if (value > 3600) problems.push(path + ": must be at most 3600");
  return problems.length === count;
}

export interface Rule {
  attribute: string;
  values: string[];
  percentage: Percentage;
}

export function checkRule(value: unknown, path: string, problems: string[]): value is Rule {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkString(value.attribute, path + ".attribute", problems);
  if (Array.isArray(value.values)) {
    value.values.forEach((item, index) => checkString(item, path + ".values[" + index + "]", problems));
  } else {
    problems.push(path + ".values: expected an array");
  }
  checkPercentage(value.percentage, path + ".percentage", problems);
  for (const key of Object.keys(value)) {
    if (!["attribute", "values", "percentage"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}

export interface FlagConfig {
  key: FlagKey;
  state: State;
  percentage: Percentage;
  rules: Rule[];
}

export function checkFlagConfig(value: unknown, path: string, problems: string[]): value is FlagConfig {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkFlagKey(value.key, path + ".key", problems);
  checkState(value.state, path + ".state", problems);
  checkPercentage(value.percentage, path + ".percentage", problems);
  if (Array.isArray(value.rules)) {
    value.rules.forEach((item, index) => checkRule(item, path + ".rules[" + index + "]", problems));
  } else {
    problems.push(path + ".rules: expected an array");
  }
  for (const key of Object.keys(value)) {
    if (!["key", "state", "percentage", "rules"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}

export interface FlagsRequest {
  environment: Environment;
}

export function checkFlagsRequest(value: unknown, path: string, problems: string[]): value is FlagsRequest {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkEnvironment(value.environment, path + ".environment", problems);
  for (const key of Object.keys(value)) {
    if (!["environment"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}

export interface FlagSet {
  flags: FlagConfig[];
}

export function checkFlagSet(value: unknown, path: string, problems: string[]): value is FlagSet {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  if (Array.isArray(value.flags)) {
    value.flags.forEach((item, index) => checkFlagConfig(item, path + ".flags[" + index + "]", problems));
  } else {
    problems.push(path + ".flags: expected an array");
  }
  for (const key of Object.keys(value)) {
    if (!["flags"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}

export interface Attribute {
  name: string;
  value: string;
}

export function checkAttribute(value: unknown, path: string, problems: string[]): value is Attribute {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkString(value.name, path + ".name", problems);
  checkString(value.value, path + ".value", problems);
  for (const key of Object.keys(value)) {
    if (!["name", "value"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}

export interface EvaluationContext {
  environment: Environment;
  user: string;
  attributes: Attribute[];
}

export function checkEvaluationContext(value: unknown, path: string, problems: string[]): value is EvaluationContext {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkEnvironment(value.environment, path + ".environment", problems);
  checkString(value.user, path + ".user", problems);
  if (Array.isArray(value.attributes)) {
    value.attributes.forEach((item, index) => checkAttribute(item, path + ".attributes[" + index + "]", problems));
  } else {
    problems.push(path + ".attributes: expected an array");
  }
  for (const key of Object.keys(value)) {
    if (!["environment", "user", "attributes"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}

export interface Evaluation {
  flag: FlagKey;
  on: boolean;
}

export function checkEvaluation(value: unknown, path: string, problems: string[]): value is Evaluation {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkFlagKey(value.flag, path + ".flag", problems);
  checkBoolean(value.on, path + ".on", problems);
  for (const key of Object.keys(value)) {
    if (!["flag", "on"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}

export interface Evaluations {
  evaluations: Evaluation[];
}

export function checkEvaluations(value: unknown, path: string, problems: string[]): value is Evaluations {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  if (Array.isArray(value.evaluations)) {
    value.evaluations.forEach((item, index) => checkEvaluation(item, path + ".evaluations[" + index + "]", problems));
  } else {
    problems.push(path + ".evaluations: expected an array");
  }
  for (const key of Object.keys(value)) {
    if (!["evaluations"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}

export interface StatusRequest {
  flag: FlagKey;
}

export function checkStatusRequest(value: unknown, path: string, problems: string[]): value is StatusRequest {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkFlagKey(value.flag, path + ".flag", problems);
  for (const key of Object.keys(value)) {
    if (!["flag"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}

export interface FlagStatus {
  flag: FlagKey;
  state: State;
  failedChecks: FailureCount;
  retryAfter?: Seconds;
}

export function checkFlagStatus(value: unknown, path: string, problems: string[]): value is FlagStatus {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkFlagKey(value.flag, path + ".flag", problems);
  checkState(value.state, path + ".state", problems);
  checkFailureCount(value.failedChecks, path + ".failedChecks", problems);
  if (value.retryAfter !== undefined) checkSeconds(value.retryAfter, path + ".retryAfter", problems);
  for (const key of Object.keys(value)) {
    if (!["flag", "state", "failedChecks", "retryAfter"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}

export interface RolloutRequest {
  flag: FlagKey;
  percentage: Percentage;
  owner: Owner;
}

export function checkRolloutRequest(value: unknown, path: string, problems: string[]): value is RolloutRequest {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkFlagKey(value.flag, path + ".flag", problems);
  checkPercentage(value.percentage, path + ".percentage", problems);
  checkOwner(value.owner, path + ".owner", problems);
  for (const key of Object.keys(value)) {
    if (!["flag", "percentage", "owner"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}

export interface KillRequest {
  flag: FlagKey;
  owner: Owner;
}

export function checkKillRequest(value: unknown, path: string, problems: string[]): value is KillRequest {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkFlagKey(value.flag, path + ".flag", problems);
  checkOwner(value.owner, path + ".owner", problems);
  for (const key of Object.keys(value)) {
    if (!["flag", "owner"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}

export interface KillResult {
  flag: FlagKey;
  killed: boolean;
}

export function checkKillResult(value: unknown, path: string, problems: string[]): value is KillResult {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkFlagKey(value.flag, path + ".flag", problems);
  checkBoolean(value.killed, path + ".killed", problems);
  for (const key of Object.keys(value)) {
    if (!["flag", "killed"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}
