<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="v" value="1"/>
  <sch:phase id="ph">
    <sch:let name="v" value="2"/>
    <sch:active pattern="p"/>
  </sch:phase>
  <sch:pattern id="p">
    <sch:rule context="item">
      <sch:assert test="$v">Holds.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
