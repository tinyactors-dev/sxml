// Doors read recently, kept in Redis for a minute.
import { createClient } from "redis";
import type { Door } from "./http.ts";

const client = createClient({ url: "redis://door-cache:6379" });

export async function getDoor(id: string): Promise<Door | undefined> {
  const text = await client.get(`door:${id}`);
  return text === null ? undefined : (JSON.parse(text) as Door);
}
