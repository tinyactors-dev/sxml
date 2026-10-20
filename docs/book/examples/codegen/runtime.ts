import type { IncomingMessage, ServerResponse } from "node:http";

// The hand-written part: what every generated file shares.
// The generated checks call these for the built-in XSD types.

/** A check adds a problem for each way the value is wrong. */
export type Check = (value: unknown, path: string, problems: string[]) => void;

/** The problems of a value that failed its check. */
export class InvalidValue extends Error {
  problems: string[];
  constructor(problems: string[]) {
    super(problems.join("; "));
    this.problems = problems;
  }
}

/** Returns the value when it passes the check, and throws otherwise. */
export function expect<T>(value: unknown, check: Check): T {
  const problems: string[] = [];
  check(value, "input", problems);
  if (problems.length > 0) throw new InvalidValue(problems);
  return value as T;
}

export function checkString(value: unknown, path: string, problems: string[]): value is string {
  if (typeof value === "string") return true;
  problems.push(`${path}: expected a string`);
  return false;
}

export function checkInteger(value: unknown, path: string, problems: string[]): value is number {
  if (Number.isInteger(value)) return true;
  problems.push(`${path}: expected an integer`);
  return false;
}

export function checkDecimal(value: unknown, path: string, problems: string[]): value is number {
  if (Number.isFinite(value)) return true;
  problems.push(`${path}: expected a number`);
  return false;
}

export function checkBoolean(value: unknown, path: string, problems: string[]): value is boolean {
  if (typeof value === "boolean") return true;
  problems.push(`${path}: expected true or false`);
  return false;
}

export function checkDate(value: unknown, path: string, problems: string[]): value is string {
  if (typeof value === "string" && /^\d{4}-\d{2}-\d{2}$/.test(value)) return true;
  problems.push(`${path}: expected a date such as 2026-10-20`);
  return false;
}

export function checkObject(value: unknown, path: string, problems: string[]): value is Record<string, unknown> {
  if (typeof value === "object" && value !== null && !Array.isArray(value)) return true;
  problems.push(`${path}: expected an object`);
  return false;
}

/** XSD counts characters, not UTF-16 code units. */
export function characterCount(value: string): number {
  return [...value].length;
}

// The client side.

/** Where the client sends its calls; a browser page keeps "/api". */
export const settings = { endpoint: "/api" };

/** Calls a remote function: a query with GET, a command with POST. */
export async function call(name: string, kind: "query" | "command", input: unknown): Promise<unknown> {
  const url = `${settings.endpoint}/${name}`;
  const response = kind === "query"
    ? await fetch(`${url}?input=${encodeURIComponent(JSON.stringify(input))}`)
    : await fetch(url, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(input),
      });
  const body = await response.json();
  if (!response.ok) throw new InvalidValue(body.problems ?? [`${name}: ${response.status}`]);
  return body;
}

// The server side.

/** One remote function, as the server sees it. */
export interface Route {
  kind: "query" | "command";
  checkInput: Check;
  checkOutput: Check;
  run: (input: any) => Promise<unknown>;
}

interface Reply {
  status: number;
  body: unknown;
}

/** Answers one request: finds the route, checks the input, runs it, checks the output. */
export async function answer(routes: Record<string, Route>, method: string, url: URL, body: string): Promise<Reply> {
  const name = url.pathname.startsWith("/api/") ? url.pathname.slice(5) : "";
  const route = Object.hasOwn(routes, name) ? routes[name] : undefined;
  if (route === undefined) return { status: 404, body: { problems: [`no function ${url.pathname}`] } };
  const expected = route.kind === "query" ? "GET" : "POST";
  if (method !== expected) return { status: 405, body: { problems: [`use ${expected}`] } };
  let input: unknown;
  try {
    input = JSON.parse(route.kind === "query" ? url.searchParams.get("input") ?? "" : body);
  } catch {
    return { status: 400, body: { problems: ["input: expected JSON"] } };
  }
  const problems: string[] = [];
  route.checkInput(input, "input", problems);
  if (problems.length > 0) return { status: 400, body: { problems } };
  const output = await route.run(input);
  route.checkOutput(output, "output", problems);
  if (problems.length > 0) return { status: 500, body: { problems } };
  return { status: 200, body: output };
}

/** A Node.js request listener for the routes. */
export function serve(routes: Record<string, Route>) {
  return async (request: IncomingMessage, response: ServerResponse) => {
    let body = "";
    for await (const chunk of request) body += chunk;
    const url = new URL(request.url ?? "/", "http://localhost");
    let reply: Reply;
    try {
      reply = await answer(routes, request.method ?? "GET", url, body);
    } catch (error) {
      reply = { status: 500, body: { problems: [String(error)] } };
    }
    response.writeHead(reply.status, { "content-type": "application/json" });
    response.end(JSON.stringify(reply.body));
  };
}
