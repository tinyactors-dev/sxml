<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="c" value="count(*)"/>
  <sch:let name="items" value="count(//item)"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="@n = $items">Last of <sch:value-of select="$items"/>, root children <sch:value-of select="$c"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
