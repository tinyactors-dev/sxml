xquery version "4.0";

(:~
 : Draws a diagram description as SVG.
 :
 : A diagram is a `diagram` element (no namespace) with a `title`, `node`
 : elements (id, label, optional kind="store", optional class) and `edge`
 : elements (from, to, optional label, optional class).
 :
 : The layout is layered, in the five phases of the Sugiyama method as the
 : layered algorithm of the Eclipse Layout Kernel (ELK) runs them: break
 : the cycles, put the nodes in layers, order each layer, place the
 : vertices, route the edges. Wherever two choices are equally good, the
 : order of the description decides, as Domrös, Riepe and von Hanxleden
 : describe in "Model Order in Sugiyama Layouts" (IVAPP 2023).
 :)
module namespace draw = "urn:example:draw";

(: ANCHOR: sizes :)
(: The sizes of the drawing, in SVG user units. :)
declare variable $draw:box-height := 40;
declare variable $draw:box-min-width := 100;
declare variable $draw:gap-across := 30;
declare variable $draw:gap-down := 24;
declare variable $draw:loop-width := 26;
declare variable $draw:padding := 20;

(:~ The width of a text in a sans-serif font of the given size: an
    estimate, from the average width of a letter. :)
declare function draw:text-width($text as xs:string?, $size as xs:integer) as xs:integer {
  xs:integer(ceiling(string-length($text) * $size * 0.56))
};

(: A vertex of the layered graph: a node of the diagram, the label of an
   edge, or a bend where an edge crosses a layer. Edges run along its x;
   left and right are how far it reaches on either side of that line. :)
declare record draw:vertex(
  id as xs:string,
  kind as xs:string,
  key as xs:decimal,
  left as xs:integer,
  right as xs:integer,
  height as xs:integer
);

(: A link of the layered graph: one step of the edge at that position
   in the description, from an upper vertex to a lower one. :)
declare record draw:link(from as xs:string, to as xs:string, edge as xs:integer);

(: A point, and a box: the top left corner and the size. :)
declare record draw:place(x as xs:integer, y as xs:integer);
declare record draw:box(x as xs:integer, y as xs:integer, width as xs:integer, height as xs:integer);

(: The route of an edge: an SVG path, and where its label goes. :)
declare record draw:route(path as xs:string, label as draw:place?);

(: A computed layout: the size of the drawing, a box per node id, and a
   route per edge, by the edge's position in the description. :)
declare record draw:layout(
  width as xs:integer,
  height as xs:integer,
  boxes as map(xs:string, draw:box),
  routes as map(xs:integer, draw:route)
);
(: ANCHOR_END: sizes :)

(: ANCHOR: style :)
(: The style rules every drawing has. :)
declare variable $draw:style := `
    .node rect {{ fill: #eef3fb; stroke: #3b5b92; stroke-width: 1.5 }}
    .store rect {{ fill: #f6efe3; stroke: #8a6a2f }}
    .node text {{ font: 14px sans-serif; fill: #1b1b1b }}
    .edge path {{ fill: none; stroke: #555555; stroke-width: 1.5 }}
    .edge text {{ font: 12px sans-serif; fill: #555555 }}`;

(: Rules a drawing has only when it uses the class. :)
declare variable $draw:class-rules := {
  "added": ".added rect, .added path { stroke: #2e7d32; stroke-width: 3 }",
  "removed": ".removed rect, .removed path { stroke: #c62828; stroke-dasharray: 6 4 }",
  "changed": ".changed rect, .changed path { stroke: #b26a00; stroke-width: 3 }"
};
(: ANCHOR_END: style :)

(: ANCHOR: cycles :)
(:~ One step of the depth-first search of draw:reversed: visit a node,
    then follow its edges in their order, and note each edge that leads
    back to a node the search is still inside. :)
declare %private function draw:visit(
  $diagram as element(diagram),
  $id as xs:string,
  $search as map(*)
) as map(*) {
  let $inside := map:put($search, "path", ($search?path, $id))
  let $done := fold-left(
    for $edge at $position in $diagram/edge
    where $edge/@from = $id and $edge/@to != $id
    return [ $position, $edge/@to/string() ],
    map:put($inside, "seen", ($inside?seen, $id)),
    fn($search, $edge) {
      if ($edge(2) = $search?path) then map:put($search, "back", ($search?back, $edge(1)))
      else if ($edge(2) = $search?seen) then $search
      else draw:visit($diagram, $edge(2), $search)
    }
  )
  return map:put($done, "path", $search?path)
};

(:~ Phase 1. The positions of the edges to reverse so that no path leads
    in a circle. A depth-first search starts from each node not yet seen,
    in the order of the description, and follows the edges in their
    order; an edge back to a node on the way it came closes a circle,
    and is reversed. A diagram without circles keeps all its edges as
    they are, and in one with circles the order of the description
    decides which edge turns: the first node is on top. :)
declare function draw:reversed($diagram as element(diagram)) as xs:integer* {
  sort(fold-left($diagram/node/@id/string(),
    { "seen": (), "path": (), "back": () },
    fn($search, $id) {
      if ($id = $search?seen) then $search else draw:visit($diagram, $id, $search)
    }
  )?back)
};
(: ANCHOR_END: cycles :)

(: ANCHOR: graph :)
(:~ The graph to lay out: a vertex per node, sized for its label (and its
    loops, drawn to its right), a vertex for the label of every edge
    between two nodes, and the edges as links, the reversed ones turned
    round. A label is a vertex of its own so that it gets a place of its
    own, as ELK's label dummies do. A vertex's key is its place in the
    description; a label sits just after the node its edge leaves. :)
declare function draw:graph($diagram as element(diagram)) as map(*) {
  let $reversed := draw:reversed($diagram)
  let $order := map:merge(for $node at $n in $diagram/node return { $node/@id: $n })
  let $nodes := (
    for $node at $n in $diagram/node
    let $ends := max((count($diagram/edge[@from = $node/@id and @to != $node/@id]),
                      count($diagram/edge[@to = $node/@id and @from != $node/@id])))
    let $width := max(($draw:box-min-width, draw:text-width($node/@label, 14) + 24,
                       ($ends + 1) * 16))
    let $loops := $diagram/edge[@from = $node/@id and @to = $node/@id]
    let $loop-width := if (empty($loops)) then 0 else
      $draw:loop-width + max((0, $loops/@label ! (draw:text-width(., 12) + 4)))
    return draw:vertex($node/@id, "node", $n, $width idiv 2,
                       $width - $width idiv 2 + $loop-width, $draw:box-height)
  )
  let $steps := (
    for $edge at $e in $diagram/edge
    where $edge/@from != $edge/@to
    let $ends := if ($e = $reversed) then ($edge/@to, $edge/@from) else ($edge/@from, $edge/@to)
    let $label := $edge/@label ! draw:vertex(`label-{ $e }`, "label",
      $order($edge/@from) + 0.5, 1, draw:text-width(., 12) + 6, 16)
    return {
      "labels": $label,
      "links": if (exists($label)) then (
        draw:link($ends[1], $label?id, $e), draw:link($label?id, $ends[2], $e)
      ) else draw:link($ends[1], $ends[2], $e)
    }
  )
  return { "vertices": ($nodes, $steps?labels), "links": $steps?links }
};
(: ANCHOR_END: graph :)

(: ANCHOR: layers :)
(:~ Phase 2. The layer of every vertex: the length of the longest path
    that leads to it, so that every link goes down. A vertex that nothing
    leads to then moves down to just above the highest vertex it leads
    to, so that its links stay short. :)
declare function draw:layers($graph as map(*)) as map(xs:string, xs:integer) {
  let $ids := $graph?vertices?id
  let $links := $graph?links
  let $longest := while-do(
    map:build($ids, value := fn { 0 }),
    fn($layers) { some $link in $links satisfies $layers($link?to) <= $layers($link?from) },
    fn($layers) {
      map:build($ids, value := fn($id) {
        max((0, $links[?to = $id] ! ($layers(?from) + 1)))
      })
    }
  )
  let $lowered := map:build($ids, value := fn($id) {
    if (empty($links[?to = $id]) and exists($links[?from = $id]))
    then min($links[?from = $id] ! $longest(?to)) - 1
    else $longest($id)
  })
  let $top := min(map:items($lowered))
  return map:build($ids, value := fn($id) { $lowered($id) - $top })
};

(:~ The graph with a bend vertex wherever a link passes a layer, so that
    every link joins two neighbouring layers. :)
declare function draw:proper($graph as map(*), $layers as map(xs:string, xs:integer)) as map(*) {
  let $keys := map:build($graph?vertices, fn($vertex) { $vertex?id }, fn($vertex) { $vertex?key })
  let $steps := (
    for $link in $graph?links
    let $from := $layers($link?from)
    let $to := $layers($link?to)
    let $bends := ($from + 1 to $to - 1) ! `bend-{ $link?edge }-{ . }`
    let $chain := ($link?from, $bends, $link?to)
    return {
      "bends": $bends ! draw:vertex(., "bend", $keys($link?from) + 0.5, 1, 1, 0),
      "layers": for $bend at $b in $bends return { $bend: $from + $b },
      "links": for $step in 1 to count($chain) - 1
        return draw:link($chain[$step], $chain[$step + 1], $link?edge)
    }
  )
  return {
    "vertices": ($graph?vertices, $steps?bends),
    "links": $steps?links,
    "layers": map:merge(($layers, $steps?layers))
  }
};
(: ANCHOR_END: layers :)

(: ANCHOR: order :)
(:~ The number of links between two layers that cross: two links cross
    when their upper ends and their lower ends are in opposite orders. :)
declare function draw:crossings($links as draw:link*, $positions as map(xs:string, xs:integer)) as xs:integer {
  count(
    for $first at $f in $links
    for $second in $links[position() > $f]
    where ($positions($first?from) - $positions($second?from))
        * ($positions($first?to) - $positions($second?to)) < 0
    return 1
  )
};

(:~ The positions of the ids of a sequence of layers: map from id to its
    place in its layer, from 1. :)
declare function draw:positions($orders as array(xs:string)*) as map(xs:string, xs:integer) {
  map:merge(for $layer in $orders for member $id at $p in $layer return { $id: $p })
};

(:~ One layer, sorted by the barycentre of each vertex: the mean position
    of its neighbours in the layer next to it. A vertex without
    neighbours there keeps its own position, and the sort is stable, so
    a tie keeps the order the layer had. :)
declare function draw:sort-layer(
  $layer as array(xs:string),
  $neighbours as fn(xs:string) as xs:string*,
  $positions as map(xs:string, xs:integer)
) as array(xs:string) {
  let $own := draw:positions($layer)
  return array {
    sort($layer?*, (), fn($id) {
      let $around := $neighbours($id)
      return if (empty($around)) then $own($id) else avg($around ! $positions(.))
    })
  }
};

(:~ Phase 3. The vertices of every layer, top to bottom, in the order
    that crosses the fewest links. The first order is the order of the
    description; then sweeps go down the layers, sorting each by the one
    above, and up again, sorting each by the one below. The best order
    any sweep finds is kept, and the first one wins a tie. :)
declare function draw:orders($proper as map(*)) as array(xs:string)* {
  let $layers := $proper?layers
  let $links := $proper?links
  let $vertices := $proper?vertices
  let $count := max(map:items($layers)) + 1
  let $first := (0 to $count - 1) ! (
    let $layer := .
    return array { sort($vertices[$layers(?id) = $layer], (), fn($v) { $v?key })?id }
  )
  let $total := fn($orders) {
    let $positions := draw:positions($orders)
    return sum(for $layer in 0 to $count - 2
      return draw:crossings($links[$layers(?from) = $layer], $positions))
  }
  let $above := fn($id) { $links[?to = $id]?from }
  let $below := fn($id) { $links[?from = $id]?to }
  let $sweep := fn($orders, $down) {
    let $range := if ($down) then 2 to $count else reverse(1 to $count - 1)
    return fold-left($range, $orders, fn($done, $index) {
      let $positions := draw:positions($done)
      let $sorted := draw:sort-layer($done[$index],
        if ($down) then $above else $below, $positions)
      return ($done[position() < $index], $sorted, $done[position() > $index])
    })
  }
  let $result := fold-left(1 to 8, { "best": $first, "crossings": $total($first), "now": $first },
    fn($state, $pass) {
      let $next := $sweep($state?now, $pass mod 2 = 1)
      let $crossings := $total($next)
      return if ($crossings < $state?crossings)
        then { "best": $next, "crossings": $crossings, "now": $next }
        else map:put($state, "now", $next)
    })
  return $result?best
};
(: ANCHOR_END: order :)

(: ANCHOR: place :)
(:~ The x of each vertex of one layer, in its order, as close as it can
    be to where it wants to be while keeping the gap to its neighbours.
    Shifting each x by the room the vertices before it need turns the
    gaps into "never smaller than the one before"; the closest such
    sequence comes from pooling neighbours that are out of order into a
    block at their mean (isotonic regression by pooling adjacent
    violators). :)
declare function draw:pack($widths as array(xs:integer)*, $wants as xs:decimal*) as xs:decimal* {
  let $offsets := fold-left(2 to count($widths), 0, fn($offsets, $i) {
    $offsets, $offsets[last()] + $widths[$i - 1](2) + $draw:gap-across + $widths[$i](1)
  })
  let $targets := for $i in 1 to count($wants) return $wants[$i] - $offsets[$i]
  let $blocks := fold-left($targets, (), fn($blocks, $target) {
    while-do(
      ($blocks, { "sum": $target, "count": 1 }),
      fn($all) {
        count($all) > 1 and $all[last() - 1]?sum div $all[last() - 1]?count
                          > $all[last()]?sum div $all[last()]?count
      },
      fn($all) {
        let $a := $all[last() - 1]
        let $b := $all[last()]
        return ($all[position() < last() - 1],
                { "sum": $a?sum + $b?sum, "count": $a?count + $b?count })
      }
    )
  })
  let $pooled := $blocks ! (let $mean := ?sum div ?count return (1 to ?count) ! $mean)
  return for $i in 1 to count($wants) return $pooled[$i] + $offsets[$i]
};

(:~ Phase 4. The x of every vertex. The vertices start packed from the
    left; then passes go down and up the layers, each vertex wanting to
    be at the mean x of its neighbours in the layer just done. A bend has
    one neighbour on each side, so it lines up, and long edges come out
    straight. :)
declare function draw:xs($proper as map(*), $orders as array(xs:string)*) as map(xs:string, xs:decimal) {
  let $vertices := map:build($proper?vertices, fn($v) { $v?id })
  let $links := $proper?links
  let $widths := fn($layer) { $layer?* ! [ $vertices(.)?left, $vertices(.)?right ] }
  let $start := map:merge(
    for $layer in $orders
    let $packed := draw:pack($widths($layer), $layer?* ! 0)
    for member $id at $p in $layer
    return { $id: $packed[$p] }
  )
  let $count := count($orders)
  return fold-left(1 to 8, $start, fn($xs, $pass) {
    let $down := $pass mod 2 = 1
    let $range := if ($down) then 2 to $count else reverse(1 to $count - 1)
    return fold-left($range, $xs, fn($xs, $index) {
      let $layer := $orders[$index]
      let $wants := $layer?* ! (
        let $id := .
        let $around := if ($down) then $links[?to = $id]?from else $links[?from = $id]?to
        return if (empty($around)) then $xs($id) else avg($around ! $xs(.))
      )
      let $packed := draw:pack($widths($layer), $wants)
      return map:merge((for member $id at $p in $layer return { $id: $packed[$p] }, $xs))
    })
  })
};
(: ANCHOR_END: place :)

(: ANCHOR: route :)
(:~ The path of an edge from a node to itself: a loop on the right of
    the box, and the place of its label beside it. :)
declare function draw:loop($box as draw:box) as draw:route {
  let $side := $box?x + $box?width
  let $out := $draw:loop-width - 4
  return draw:route(
    `M { $side } { $box?y + 10 } C { $side + $out } { $box?y + 2 }, { $side + $out } { $box?y + $box?height - 2 }, { $side } { $box?y + $box?height - 10 }`,
    draw:place($side + $draw:loop-width, $box?y + $box?height idiv 2 + 4)
  )
};

(:~ The SVG path through a sequence of points. Between layers the path
    curves (a cubic Bézier curve that leaves and arrives vertically);
    inside a layer, where it passes a label or a bend, it goes straight
    down. :)
declare function draw:path($points as draw:place*, $straight as xs:boolean*) as xs:string {
  string-join((
    `M { $points[1]?x } { $points[1]?y }`,
    for $i in 1 to count($points) - 1
    let $a := $points[$i]
    let $b := $points[$i + 1]
    let $middle := ($a?y + $b?y) idiv 2
    return if ($straight[$i]) then `L { $b?x } { $b?y }`
      else `C { $a?x } { $middle }, { $b?x } { $middle }, { $b?x } { $b?y }`
  ), " ")
};
(: ANCHOR_END: route :)

(: ANCHOR: layout :)
(:~ The layout of a diagram: all five phases. Phase 5, the routes: an
    edge leaves the bottom of its upper box and enters the top of its
    lower one, at points spread along the side in the order of the other
    ends, so that edges on one side do not cross there. It passes its
    label straight down, curves through its bends, and a reversed edge is drawn the
    other way round, so that its arrow points where the edge goes. :)
declare function draw:layout($diagram as element(diagram)) as draw:layout {
  let $graph := draw:graph($diagram)
  let $proper := draw:proper($graph, draw:layers($graph))
  let $orders := draw:orders($proper)
  let $xs := draw:xs($proper, $orders)
  let $vertices := map:build($proper?vertices, fn($v) { $v?id })
  let $links := $proper?links
  let $layers := $proper?layers
  let $shift := $draw:padding - min($proper?vertices ! ($xs(?id) - ?left))
  let $x := fn($id) { xs:integer(round($xs($id) + $shift)) }
  let $heights := $orders ! max((0, ?* ! $vertices(.)?height))
  let $tops := fold-left($heights, (), fn($tops, $height) {
    $tops, if (empty($tops)) then $draw:padding
           else $tops[last()] + $heights[count($tops)] + $draw:gap-down
  })
  let $band := fn($id) {
    let $layer := $layers($id) + 1
    return ($tops[$layer], $tops[$layer] + $heights[$layer])
  }
  let $boxes := map:merge(
    for $vertex in $proper?vertices[?kind = "node"]
    let $width := 2 * $vertex?left
    let $top := $band($vertex?id)[1] + ($heights[$layers($vertex?id) + 1] - $vertex?height) idiv 2
    return { $vertex?id: draw:box($x($vertex?id) - $vertex?left, $top, $width, $vertex?height) }
  )
  let $port := fn($id, $other, $bottom) {
    let $box := $boxes($id)
    let $ends := sort(if ($bottom) then $links[?from = $id]?to else $links[?to = $id]?from,
                      (), fn($end) { $xs($end) })
    let $k := index-of($ends, $other)[1]
    return draw:place($box?x + $box?width * $k idiv (count($ends) + 1),
                      if ($bottom) then $box?y + $box?height else $box?y)
  }
  let $reversed := draw:reversed($diagram)
  let $routes := map:merge((
    for $edge at $e in $diagram/edge
    where $edge/@from = $edge/@to
    return { $e: draw:loop($boxes($edge/@from)) },
    for $edge at $e in $diagram/edge
    where $edge/@from != $edge/@to
    let $steps := sort($links[?edge = $e], (), fn($link) { $layers($link?from) })
    let $chain := ($steps[1]?from, $steps?to)
    let $inner := $chain[position() = 2 to last() - 1]
    let $points := (
      $port($chain[1], $chain[2], true()),
      for $id in $inner
      return if (starts-with($id, "label-"))
        then (draw:place($x($id), $band($id)[1]), draw:place($x($id), $band($id)[2]))
        else draw:place($x($id), sum($band($id)) idiv 2),
      $port($chain[last()], $chain[last() - 1], false())
    )
    let $straight := (false(), $inner ! (if (starts-with(., "label-")) then (true(), false()) else false()))
    let $label := $inner[starts-with(., "label-")] ! draw:place($x(.) + 5, sum($band(.)) idiv 2 + 4)
    return { $e: draw:route(
      if ($e = $reversed) then draw:path(reverse($points), reverse($straight))
      else draw:path($points, $straight),
      $label
    ) }
  ))
  let $width := max($proper?vertices ! ($x(?id) + ?right)) + $draw:padding
  let $height := $tops[last()] + $heights[last()] + $draw:padding
  return draw:layout($width, $height, $boxes, $routes)
};
(: ANCHOR_END: layout :)

(: ANCHOR: node :)
(:~ One node: a group with a box and its label. :)
declare function draw:node($node as element(node), $box as draw:box) as element() {
  let $kind := ($node/@kind, "service")[1]
  return <g xmlns="http://www.w3.org/2000/svg"
            class="{ "node", $kind, $node/@class }" id="node-{ $node/@id }">
    <rect x="{ $box?x }" y="{ $box?y }" width="{ $box?width }" height="{ $box?height }"
          rx="{ if ($kind = "store") then 20 else 6 }"/>
    <text x="{ $box?x + $box?width idiv 2 }" y="{ $box?y + 25 }"
          text-anchor="middle">{ $node/@label/string() }</text>
  </g>
};
(: ANCHOR_END: node :)

(: ANCHOR: edge :)
(:~ One edge: a group with its path and its label. :)
declare function draw:edge($edge as element(edge), $route as draw:route) as element() {
  <g xmlns="http://www.w3.org/2000/svg"
     class="{ "edge", $edge/@class }" id="edge-{ $edge/@from }-{ $edge/@to }">
    <path d="{ $route?path }" marker-end="url(#arrow)"/>
    { $route?label ! <text x="{ ?x }" y="{ ?y }">{ $edge/@label/string() }</text> }
  </g>
};
(: ANCHOR_END: edge :)

(: ANCHOR: svg :)
(:~ The drawing of a diagram: edges first, so that boxes cover their ends. :)
declare function draw:svg($diagram as element(diagram)) as element() {
  let $layout := draw:layout($diagram)
  let $title := $diagram/@title/string()
  let $nodes := $diagram/node
  let $edges := $diagram/edge
  let $features := distinct-values(($nodes, $edges)/@class ! tokenize(.))
  return <svg xmlns="http://www.w3.org/2000/svg" width="{ $layout?width }"
              height="{ $layout?height }" viewBox="0 0 { $layout?width } { $layout?height }">
    <title>{ $title }</title>
    <style>{
      string-join(($draw:style, $features ! $draw:class-rules(.)), "&#10;    ") || "&#10;  "
    }</style>
    <defs>
      <marker id="arrow" viewBox="0 0 10 10" refX="10" refY="5"
              markerWidth="9" markerHeight="9" markerUnits="userSpaceOnUse" orient="auto">
        <path d="M 0 0 L 10 5 L 0 10 z" fill="#555555"/>
      </marker>
    </defs>
    { for $edge at $e in $edges return draw:edge($edge, $layout?routes($e)) }
    { $nodes ! draw:node(., $layout?boxes(@id)) }
  </svg>
};
(: ANCHOR_END: svg :)
