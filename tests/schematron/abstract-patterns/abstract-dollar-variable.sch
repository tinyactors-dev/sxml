<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="vars">
    <sch:rule context="$element">
      <sch:let name="kids" value="count(*)"/>
      <sch:assert test="$kids &gt; 0">Has <sch:value-of select="$kids"/> children.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="vars">
    <sch:param name="element" value="row"/>
  </sch:pattern>
</sch:schema>
