<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="visit">
    <sch:param name="items"/>
    <sch:rule context="list" visit-each="$items">
      <sch:report test="true()">Big <sch:value-of select="@v"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="visit">
    <sch:param name="items" value="entry[@v &gt; 5]"/>
  </sch:pattern>
</sch:schema>
