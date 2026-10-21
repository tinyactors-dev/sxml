// The lock registry knows every door and its state.
import { request, type Door } from "./http.ts";

export async function getDoors(): Promise<Door[]> {
  return (await request("lock-registry", "/doors")) as Door[];
}

export async function getDoor(id: string): Promise<Door> {
  return (await request("lock-registry", `/doors/${id}`)) as Door;
}

export async function setState(id: string, state: Door["state"]): Promise<void> {
  await request("lock-registry", `/doors/${id}/state`, {
    method: "PUT",
    body: JSON.stringify({ state }),
  });
}
