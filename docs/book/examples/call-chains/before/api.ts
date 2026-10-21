// The HTTP entry points of the Vane service: the remote functions the
// SDKs and the console call (here the two that the change touches).
import { createServer } from "node:http";
import { listFlags, killFlag } from "./flags.ts";

async function route(method: string, path: string, body: string): Promise<unknown> {
  if (method === "GET" && path.startsWith("/api/getFlags")) {
    return { flags: await listFlags() };
  }
  if (method === "POST" && path === "/api/killFlag") {
    const { flag, owner } = JSON.parse(body) as { flag: string; owner: string };
    return { flag, killed: await killFlag(flag, owner) };
  }
  return { problems: [`no function ${path}`] };
}

export function start(port: number): void {
  createServer(async (request, response) => {
    let body = "";
    for await (const chunk of request) {
      body += chunk;
    }
    const result = await route(request.method ?? "GET", request.url ?? "/", body);
    response.setHeader("content-type", "application/json");
    response.end(JSON.stringify(result));
  }).listen(port);
}
