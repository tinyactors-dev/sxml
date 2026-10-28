<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="p">
    <sch:param name="limit" value="1"/>
    <sch:active pattern="a"/>
  </sch:phase>
  <sch:pattern id="a">
    <sch:rule context="item">
      <sch:assert test="true()">Always.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
