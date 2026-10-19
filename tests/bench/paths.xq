(: Path expressions over the auction document: descendant steps, steps
   that need no sort into document order, and steps that do. :)
(
  count(//item//text()),
  count(//person/name),
  count(/site/regions/*/item/description//keyword),
  count(//open_auction/bidder/increase),
  count(//item/../..),
  count((//person, //item)/name),
  sum(//closed_auction/price)
)
