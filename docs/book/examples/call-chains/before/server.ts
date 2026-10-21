// The HTTP entry points of the lock service.
import { createServer } from "node:http";
import { listDoors, unlockDoor } from "./doors.ts";

async function route(method: string, path: string, body: string): Promise<unknown> {
  if (method === "GET" && path === "/doors") {
    return listDoors();
  }
  const unlock = path.match(/^\/doors\/([^/]+)\/unlock$/);
  if (method === "POST" && unlock !== null) {
    const { code } = JSON.parse(body) as { code: string };
    return { unlocked: await unlockDoor(unlock[1], code) };
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
