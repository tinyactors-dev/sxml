<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="rows">
    <sch:rule context="$element">
      <sch:assert test="$condition">The condition holds.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="rows">
    <sch:param name="element" value="row"/>
    <sch:param name="condition" value="count(cell) = 1 and not(@hidden)"/>
  </sch:pattern>
</sch:schema>
