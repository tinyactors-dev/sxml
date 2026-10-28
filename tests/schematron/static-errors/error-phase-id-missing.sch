<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase><sch:active pattern="p1"/></sch:phase>
  <sch:pattern id="p1">
    <sch:rule context="item">
      <sch:assert test="@n">An item has an n attribute.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
