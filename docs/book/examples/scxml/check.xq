(: Checks of a Latch machine that its schemas cannot make. The context
   item is the machine; the draft it implements is latch.xml. Prints one
   line for each problem, and nothing when there is none. :)
import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
declare namespace latch = "urn:example:latch";
declare namespace xsi = "http://www.w3.org/2001/XMLSchema-instance";
declare option output:method "text";
declare option output:item-separator "&#10;";

declare variable $machine := validate { / };
declare variable $draft := doc("latch.xml");

(: Every state the machine can get to: start with the initial state, and
   add the targets of the reached states' transitions, once per state. :)
declare function local:reachable() as xs:string* {
  let $states := $machine//(sc:state | sc:final)
  return fold-left(1 to count($states), string($machine/sc:scxml/@initial),
    fn($reached, $pass) {
      distinct-values((
        $reached,
        $states[@id = $reached]//sc:transition/@target ! string(),
        $states[@id = $reached]/@initial ! string()
      ))
    })
};

let $declared := $machine//latch:event/@name ! string()
let $handled := $machine//sc:transition/data(@event) ! string()
let $sent := $machine//(sc:send | sc:raise)/@event ! string()
let $reachable := local:reachable()
let $anchors := $draft//t/@anchor ! string()
let $implemented := $machine//@latch:implements ! data()
let $schema := doc("latch-data.xsd")/xs:schema
let $payloads := $schema/xs:element/@name ! QName($schema/@targetNamespace, .)
return (
  (: States that no path from the initial state reaches. :)
  $machine//sc:state[not(@id = $reachable)] ! `unreachable state: {@id}`,

  (: Events a transition waits for that the machine does not declare. :)
  distinct-values($handled[not(. = $declared)]) ! `undeclared event: {.}`,

  (: Declared events that nothing handles, sends or raises. :)
  $declared[not(. = ($handled, $sent))] ! `unused event: {.}`,

  (: Payloads that name no global element of latch-data.xsd. :)
  $machine//latch:event[@latch:payload]
    [not(data(@latch:payload) = $payloads)]
    ! `unknown payload: {@latch:payload} of {@name}`,

  (: Data items whose type the schema does not have, or whose value is
     not one of their type. :)
  for $data in $machine//sc:data
  let $type := data($data/@latch:type)
  return if (empty(function-lookup($type, 1))) then
    `unknown type: {$data/@latch:type} of {$data/@id}`
  else try {
    (validate {
      element latch:value {
        namespace type { namespace-uri-from-QName($type) },
        attribute xsi:type { "type:" || local-name-from-QName($type) },
        string($data/@expr)
      }
    }) ! ()
  } catch err:XQDY0027 {
    `bad value: {$data/@id} = {$data/@expr} is not a {$data/@latch:type}`
  },

  (: Anchors the machine names that the draft does not have. :)
  distinct-values($implemented[not(. = $anchors)]) ! `unknown requirement: {.}`,

  (: The draft's MUST requirements that nothing in the machine implements. :)
  $draft//t[bcp14 = ("MUST", "MUST NOT")][not(@anchor = $implemented)]
    ! `not implemented: {@anchor} ({bcp14})`
)
