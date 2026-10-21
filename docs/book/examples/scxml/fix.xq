(: Fixes the flaws check.xq finds in lock.scxml, and changes nothing else. :)
declare default element namespace "http://www.w3.org/2005/07/scxml";
declare namespace latch = "urn:example:latch";

. transform with {
  (: The machine as a whole keeps a lock in exactly one state. :)
  insert node attribute latch:implements { "req-1" } into scxml,

  (: Declare the event the unlocked state handles. :)
  insert node
    <latch:event name="door.opened">
      <p xmlns="http://www.w3.org/1999/xhtml">The door sensor reports that the door opened.</p>
    </latch:event>
    after .//latch:event[@name = "timeout.relock"],

  (: A jammed bolt leads to the jammed state. :)
  for $state in .//state[@id = ("locked", "unlocked")]
  return insert node <transition event="bolt.jammed" target="jammed"/>
    as first into $state
}
