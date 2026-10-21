xquery version "4.0";

(:~
 : Draws a diagram description as SVG.
 :
 : A diagram is a `diagram` element (no namespace) with a `title`, `node`
 : elements (id, label, optional kind="store", optional class) and `edge`
 : elements (from, to, optional label, optional class). The layout is
 : computed: a node's rank is its breadth-first distance from the start
 : nodes (the first node and every node without incoming edges); nodes the
 : search never reaches go into one last row. Each rank is a row, nodes in
 : document order, each row centred.
 :)
module namespace draw = "urn:example:draw";

(: The sizes of the drawing, in SVG user units. :)
declare variable $draw:box-width := 140;
declare variable $draw:box-height := 40;
declare variable $draw:gap-across := 40;
declare variable $draw:gap-down := 60;
declare variable $draw:padding := 20;

(: The top left corner of a node's box. :)
declare record draw:place(x as xs:integer, y as xs:integer);

(: A computed layout: the size of the drawing and a place per node id. :)
declare record draw:layout(
  width as xs:integer,
  height as xs:integer,
  places as map(xs:string, draw:place)
);

(: The style rules every drawing has. :)
declare variable $draw:style := `
    .node rect {{ fill: #eef3fb; stroke: #3b5b92; stroke-width: 1.5 }}
    .store rect {{ fill: #f6efe3; stroke: #8a6a2f }}
    .node text {{ font: 14px sans-serif; fill: #1b1b1b }}
    .edge line {{ stroke: #555555; stroke-width: 1.5 }}
    .edge text {{ font: 12px sans-serif; fill: #555555 }}`;

(: Rules a drawing has only when it uses the class (or, for "loop", has
   an edge from a node to itself). :)
declare variable $draw:class-rules := {
  "loop": ".edge path { fill: none; stroke: #555555; stroke-width: 1.5 }",
  "added": ".added rect, .added line, .added path { stroke: #2e7d32; stroke-width: 3 }",
  "removed": ".removed rect, .removed line, .removed path { stroke: #c62828; stroke-dasharray: 6 4 }",
  "changed": ".changed rect, .changed line, .changed path { stroke: #b26a00; stroke-width: 3 }"
};

(:~ The ids of the nodes one edge away from the last rank found. :)
declare %private function draw:next(
  $diagram as element(diagram),
  $ranks as map(xs:string, xs:integer)
) as xs:string* {
  let $last := max(map:items($ranks))
  let $frontier := map:keys($ranks)[$ranks(.) = $last]
  return distinct-values($diagram/edge[@from = $frontier]/@to)
    [not(map:contains($ranks, .))]
};

(:~ The rank of every node: a map from node id to row number. :)
declare function draw:ranks($diagram as element(diagram)) as map(xs:string, xs:integer) {
  let $targets := $diagram/edge/@to
  let $starts := ($diagram/node[1], $diagram/node[not(@id = $targets)])/@id
  let $reached := while-do(
    map:build($starts, string#1, fn { 0 }),
    fn($ranks) { exists(draw:next($diagram, $ranks)) },
    fn($ranks) {
      let $rank := max(map:items($ranks)) + 1
      return map:merge(($ranks, map:build(draw:next($diagram, $ranks), value := fn { $rank })))
    }
  )
  let $rest := $diagram/node/@id[not(map:contains($reached, .))]
  return map:merge(($reached, map:build($rest, string#1, fn { max(map:items($reached)) + 1 })))
};

(:~ The layout of a diagram: the size of the drawing and where each box goes. :)
declare function draw:layout($diagram as element(diagram)) as draw:layout {
  let $ranks := draw:ranks($diagram)
  let $rows := (
    for $node in $diagram/node
    group by $rank := $ranks($node/@id)
    order by $rank
    return array { $node/@id/string() }
  )
  let $row-width := fn($row as array(*)) as xs:integer {
    array:size($row) * $draw:box-width + (array:size($row) - 1) * $draw:gap-across
  }
  let $width := 2 * $draw:padding + max($rows ! $row-width(.))
  let $height := 2 * $draw:padding + count($rows) * $draw:box-height
    + (count($rows) - 1) * $draw:gap-down
  let $places := map:merge(
    for $row at $row-number in $rows
    let $left := ($width - $row-width($row)) idiv 2
    for member $id at $position in $row
    return {
      $id: draw:place(
        $left + ($position - 1) * ($draw:box-width + $draw:gap-across),
        $draw:padding + ($row-number - 1) * ($draw:box-height + $draw:gap-down)
      )
    }
  )
  return draw:layout($width, $height, $places)
};

(:~ One node: a group with a box and its label. :)
declare function draw:node($node as element(node), $place as draw:place) as element() {
  let $kind := ($node/@kind, "service")[1]
  return <g xmlns="http://www.w3.org/2000/svg"
            class="{ "node", $kind, $node/@class }" id="node-{ $node/@id }">
    <rect x="{ $place?x }" y="{ $place?y }"
          width="{ $draw:box-width }" height="{ $draw:box-height }"
          rx="{ if ($kind = "store") then 20 else 6 }"/>
    <text x="{ $place?x + $draw:box-width idiv 2 }" y="{ $place?y + 25 }"
          text-anchor="middle">{ $node/@label/string() }</text>
  </g>
};

(:~ One edge: a group with a line (or, from a node to itself, a loop)
    and its label. :)
declare function draw:edge($edge as element(edge), $places as map(*)) as element() {
  let $from := $places($edge/@from)
  let $to := $places($edge/@to)
  let $half := $draw:box-width idiv 2
  let $line := (
    if ($edge/@from = $edge/@to) then ()
    else if ($to?y > $from?y) then        (: down: bottom centre to top centre :)
      ($from?x + $half, $from?y + $draw:box-height, $to?x + $half, $to?y)
    else if ($to?y < $from?y) then        (: up: beside the centre, so that it does
                                             not cover an edge going down :)
      ($from?x + $half + 30, $from?y, $to?x + $half + 30, $to?y + $draw:box-height)
    else if ($to?x > $from?x) then        (: across, to the right :)
      ($from?x + $draw:box-width, $from?y + 20, $to?x, $to?y + 20)
    else                                  (: across, to the left :)
      ($from?x, $from?y + 20, $to?x + $draw:box-width, $to?y + 20)
  )
  return <g xmlns="http://www.w3.org/2000/svg"
            class="{ "edge", $edge/@class }" id="edge-{ $edge/@from }-{ $edge/@to }">{
    if (empty($line)) then (
      (: a loop below the box, left of the edges that leave its centre :)
      let $left := $from?x + 20
      let $bottom := $from?y + $draw:box-height
      return (
        <path d="M { $left } { $bottom } C { $left } { $bottom + 24 }, { $left + 30 } { $bottom + 24 }, { $left + 30 } { $bottom }"
              marker-end="url(#arrow)"/>,
        $edge/@label ! <text x="{ $left + 36 }" y="{ $bottom + 20 }">{ string() }</text>
      )
    ) else (
      <line x1="{ $line[1] }" y1="{ $line[2] }" x2="{ $line[3] }" y2="{ $line[4] }"
            marker-end="url(#arrow)"/>,
      $edge/@label ! <text x="{ ($line[1] + $line[3]) idiv 2 + 6 }"
                           y="{ ($line[2] + $line[4]) idiv 2 + 4 }">{ string() }</text>
    )
  }</g>
};

(:~ The drawing of a diagram: edges first, so that boxes cover their ends. :)
declare function draw:svg($diagram as element(diagram)) as element() {
  let $layout := draw:layout($diagram)
  let $title := $diagram/@title/string()
  let $nodes := $diagram/node
  let $edges := $diagram/edge
  let $features := distinct-values((
    if ($diagram/edge[@from = @to]) { "loop" },
    $diagram/*/@class ! tokenize(.)
  ))
  return <svg xmlns="http://www.w3.org/2000/svg" width="{ $layout?width }"
              height="{ $layout?height }" viewBox="0 0 { $layout?width } { $layout?height }">
    <title>{ $title }</title>
    <style>{
      string-join(($draw:style, $features ! $draw:class-rules(.)), "&#10;    ") || "&#10;  "
    }</style>
    <defs>
      <marker id="arrow" viewBox="0 0 10 10" refX="10" refY="5"
              markerWidth="8" markerHeight="8" orient="auto">
        <path d="M 0 0 L 10 5 L 0 10 z" fill="#555555"/>
      </marker>
    </defs>
    { $edges ! draw:edge(., $layout?places) }
    { $nodes ! draw:node(., $layout?places(@id)) }
  </svg>
};
