<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="only-first"><sch:active pattern="first"/></sch:phase>
  <sch:pattern id="first" role="structure">
    <sch:title>The first pattern</sch:title>
    <sch:rule context="item" id="items" role="item-rule" flag="item-seen">
      <sch:title>Items</sch:title>
      <sch:assert test="@n != 2" id="not-two" role="warning" flag="two">Not two.</sch:assert>
    </sch:rule>
    <sch:rule context="missing" id="never">
      <sch:assert test="true()">Never fires.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="second">
    <sch:rule context="doc">
      <sch:report test="count(item) = 3" id="three">Three items.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
