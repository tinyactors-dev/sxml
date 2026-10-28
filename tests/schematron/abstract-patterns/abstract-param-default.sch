<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="bounded">
    <sch:param name="element"/>
    <sch:param name="limit" value="2"/>
    <sch:rule context="$element">
      <sch:assert test="@n &lt;= $limit">Within <sch:value-of select="$limit"/>.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="bounded">
    <sch:param name="element" value="item"/>
    <sch:param name="limit"/>
  </sch:pattern>
</sch:schema>
