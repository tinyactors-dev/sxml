// A small client for the other services of the Vane system.

export interface Flag {
  key: string;
  name: string;
  state: "stable" | "rolling-out" | "paused";
  percentage: number;
  owners: string[];
}

export async function request(service: string, path: string, init?: RequestInit): Promise<unknown> {
  const response = await fetch(`http://${service}${path}`, init);
  if (!response.ok) {
    throw new Error(`${service} answered ${response.status} for ${path}`);
  }
  return response.json();
}
