(: Fixes the flaws check.xq finds in flag.scxml, and changes nothing else. :)
declare default element namespace "http://www.w3.org/2005/07/scxml";
declare namespace vane = "urn:example:vane";

. transform with {
  (: The machine as a whole keeps a flag in exactly one state. :)
  insert node attribute vane:implements { "req-1" } into scxml,

  (: Declare the event the rolling-out state handles. :)
  insert node
    <vane:event name="flag.evaluated">
      <p xmlns="http://www.w3.org/1999/xhtml">An SDK reports that it evaluated the flag.</p>
    </vane:event>
    after .//vane:event[@name = "step.done"],

  (: An owner can archive a flag that is stable or paused. :)
  for $state in .//state[@id = ("stable", "paused")]
  return insert node <transition event="flag.archived" target="archived"/>
    as first into $state
}
