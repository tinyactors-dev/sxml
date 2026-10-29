<?xml version="1.0" encoding="UTF-8"?>
<!-- Every state can be reached from the initial state. -->
<pattern xmlns="http://purl.oclc.org/dsdl/schematron" id="states">
  <let name="reachable" value="
    let $states := //(sc:state | sc:final)
    return fold-left(1 to count($states), string(/sc:scxml/@initial),
      fn($reached, $pass) {
        distinct-values(($reached,
          $states[@id = $reached]//sc:transition/@target ! tokenize(.)))
      })"/>
  <rule context="sc:state">
    <assert id="reachable" test="@id = $reachable">No transition leads
      to the state <value-of select="@id"/>.</assert>
  </rule>
</pattern>
