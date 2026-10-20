import { formatPrice } from "./money.js";

const TAX_RATE = 0.2;

export function total(items) {
  // TODO: apply discounts
  return items.reduce((sum, item) => sum + item.price * item.quantity, 0);
}

export function totalWithTax(items) {
  return total(items) * (1 + TAX_RATE);
}

function describe(item) {
  // TODO: localize the currency
  return `${item.name}: ${formatPrice(item.price)}`;
}

export const summary = (items) => items.map(describe).join("\n");
