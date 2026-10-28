<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="scaled">
    <sch:rule context="line">
      <sch:let name="AMOUNTX" value="$AMOUNT * 10"/>
      <sch:report test="$AMOUNTX = 10">Ten.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="scaled">
    <sch:param name="AMOUNT" value="@amount"/>
  </sch:pattern>
</sch:schema>
