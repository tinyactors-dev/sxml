// What the lock service does for each request.
import * as registry from "./registry.ts";
import * as cache from "./cache.ts";
import type { Door } from "./http.ts";

export interface DoorSummary {
  id: string;
  name: string;
  state: Door["state"];
}

function summarize(door: Door): DoorSummary {
  return { id: door.id, name: door.name, state: door.state };
}

export async function listDoors(): Promise<DoorSummary[]> {
  const doors = await registry.getDoors();
  return doors.map(summarize);
}

function codeMatches(door: Door, code: string): boolean {
  return door.codes.includes(code);
}

export async function unlockDoor(id: string, code: string): Promise<boolean> {
  const door = (await cache.getDoor(id)) ?? (await registry.getDoor(id));
  if (door.state === "lockout" || !codeMatches(door, code)) {
    return false;
  }
  await registry.setState(id, "unlocked");
  return true;
}
