// The HTTP entry points of the flag service.
import { createServer } from "node:http";
import { listFlags, killFlag } from "./flags.ts";

async function route(method: string, path: string, body: string): Promise<unknown> {
  if (method === "GET" && path === "/flags") {
    return listFlags();
  }
  const kill = path.match(/^\/flags\/([^/]+)\/kill$/);
  if (method === "POST" && kill !== null) {
    const { owner } = JSON.parse(body) as { owner: string };
    return { killed: await killFlag(kill[1], owner) };
  }
  return { error: "not found" };
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
