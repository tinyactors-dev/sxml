// A small client for the other services of the Latch system.

export interface Door {
  id: string;
  name: string;
  state: "locked" | "unlocked" | "lockout";
  codes: string[];
}

export async function request(service: string, path: string, init?: RequestInit): Promise<unknown> {
  const response = await fetch(`http://${service}${path}`, init);
  if (!response.ok) {
    throw new Error(`${service} answered ${response.status} for ${path}`);
  }
  return response.json();
}
