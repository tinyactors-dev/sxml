export async function request(path: string, body: unknown) {
  const response = await fetch(`/api${path}`, {
    method: "POST",
    body: JSON.stringify(body),
  });
  return response.json();
}
