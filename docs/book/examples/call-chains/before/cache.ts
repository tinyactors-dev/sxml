// Flags read recently, kept in Redis for a minute.
import { createClient } from "redis";
import type { Flag } from "./http.ts";

const client = createClient({ url: "redis://flag-cache:6379" });

export async function getFlag(key: string): Promise<Flag | undefined> {
  const text = await client.get(`flag:${key}`);
  return text === null ? undefined : (JSON.parse(text) as Flag);
}
