<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="bounded">
    <sch:param name="element"/>
    <sch:rule context="$element">
      <sch:assert test="@n">Has n.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="bounded">
    <sch:param name="element" value="item"/>
    <sch:param name="other" value="1"/>
  </sch:pattern>
</sch:schema>
