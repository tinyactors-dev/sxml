declare option output:method "text";
declare option output:item-separator "&#10;";

for $loan in doc("loans.xml")//loan
group by $id := string($loan/@book)
let $count := count($loan)
order by $count descending, $id
count $rank
where $rank le 3
return `{$rank}. {//book[@id = $id]/title} ({$count})`
