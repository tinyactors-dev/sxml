<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="relaxed">
    <sch:let name="level" value="'warning'"/>
    <sch:active pattern="p"/>
  </sch:phase>
  <sch:phase id="strict">
    <sch:let name="level" value="'error'"/>
    <sch:active pattern="p"/>
  </sch:phase>
  <sch:pattern id="p">
    <sch:rule context="item">
      <sch:assert test="@n != 2" role="$level">Not two.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
