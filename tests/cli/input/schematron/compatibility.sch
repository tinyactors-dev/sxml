<?xml version="1.0" encoding="UTF-8"?>
<!-- The deviations real rule sets have, read with warnings (D331). -->
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern id="globals">
    <sch:let name="limit" value="10"/>
  </sch:pattern>
  <sch:pattern abstract="true" id="bounded">
    <sch:rule context="$element">
      <sch:assert test="number(@quantity) le $limit">At most <sch:value-of select="$limit"/>.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="bounded" id="lines">
    <sch:param name="element" value="line"/>
    <sch:param name="element" value="order"/>
  </sch:pattern>
  <sch:pattern id="totals">
    <sch:rule context="order">
      <sch:assert test="$total gt 0">The order is empty.</sch:assert>
      <sch:let name="total" value="sum(line/@quantity)"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>
