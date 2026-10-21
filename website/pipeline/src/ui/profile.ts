export async function loadProfile(id: string) {
  const response = await fetch(`/api/users/${id}`);
  console.log("profile", id);
  return response.json();
}
