<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="xs" uri="http://www.w3.org/2001/XMLSchema"/>
  <sch:pattern abstract="true" id="t">
    <sch:rule context="item">
      <sch:assert test="$check">Checked.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="t">
    <sch:param name="check" value="xs:integer(@n) gt 0"/>
  </sch:pattern>
</sch:schema>
