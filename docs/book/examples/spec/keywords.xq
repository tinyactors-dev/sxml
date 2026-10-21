declare option output:method "text";
declare option output:item-separator "&#10;";

for $requirement in //t[@anchor]
group by $keyword := string($requirement/bcp14)
let $count := count($requirement)
order by $count descending, $keyword
count $rank
where $rank le 3
return `{$rank}. {$keyword} ({$count}): {string-join($requirement/@anchor, ", ")}`
