<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="e" uri="urn:example:e"/>
  <sch:let name="count" value="count(//e:item)"/>
  <sch:pattern>
    <sch:rule context="/*">
      <sch:assert test="$count = 1">One item, not <sch:value-of select="$count"/>.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
