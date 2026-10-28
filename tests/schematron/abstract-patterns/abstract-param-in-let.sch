<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="counted">
    <sch:rule context="$element">
      <sch:let name="parts" value="count($part)"/>
      <sch:assert test="$parts = 1">One part, not <sch:value-of select="$parts"/>.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="counted">
    <sch:param name="element" value="row"/>
    <sch:param name="part" value="cell"/>
  </sch:pattern>
</sch:schema>
