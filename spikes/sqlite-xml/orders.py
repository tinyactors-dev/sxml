# Order documents for the benchmark: an order, its customer, addresses and lines.
import random
def order(number, rng):
    lines = "".join(
        f'\n    <line sku="SKU-{rng.randrange(1000, 9999)}" qty="{rng.randrange(1, 6)}" price="{rng.randrange(199, 9999) / 100}">'
        f'<description>{rng.choice(["Espresso beans 1kg", "Paper filters", "Grinder burr set", "Milk jug", "Descaler"])}</description></line>'
        for _ in range(rng.randrange(1, 12)))
    status = rng.choice(["paid", "paid", "paid", "pending", "refunded", "shipped"])
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<order xmlns="urn:example:orders" id="o-{number}" status="{status}" placed="2026-{rng.randrange(1, 12):02}-{rng.randrange(1, 28):02}T10:00:00Z">
  <customer id="c-{rng.randrange(1, 5000)}"><name>{rng.choice(["Ada Lovelace", "Grace Hopper", "Edsger Dijkstra", "Barbara Liskov"])}</name><email>c{number}@example.com</email></customer>
  <shipping country="{rng.choice(["DE", "EE", "US", "FR"])}"><street>{rng.randrange(1, 200)} Market Street</street><city>{rng.choice(["Tallinn", "Berlin", "Lyon", "Austin"])}</city></shipping>
  <lines>{lines}
  </lines>
  <!-- imported from the shop -->
</order>
'''
def orders(count, seed=1):
    rng = random.Random(seed)
    return [order(number, rng) for number in range(count)]
if __name__ == "__main__":
    import os, sys
    os.makedirs(sys.argv[1], exist_ok=True)
    for number, text in enumerate(orders(int(sys.argv[2]))):
        open(f"{sys.argv[1]}/{number:05}.xml", "w").write(text)
