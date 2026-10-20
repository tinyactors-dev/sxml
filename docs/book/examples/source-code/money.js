export function formatPrice(amount) {
  return amount.toFixed(2) + " EUR";
}

export function formatTotal(amounts) {
  return formatPrice(amounts.reduce((sum, amount) => sum + amount, 0));
}
