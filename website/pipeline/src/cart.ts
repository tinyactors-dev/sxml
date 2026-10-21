import { request } from "./api/client";

export function addItem(
  cart: Cart, sku: string, quantity: number, price: number,
): Cart {
  const line = { sku, quantity, price };
  return { ...cart, lines: [...cart.lines, line] };
}

export async function checkout(cart: Cart) {
  return request("/orders", cart);
}
