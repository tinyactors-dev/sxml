<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="amounts">
    <sch:rule context="$ELEMENT">
      <sch:report test="$AMOUNTX &gt; $AMOUNT">The second amount is larger.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="amounts">
    <sch:param name="ELEMENT" value="line"/>
    <sch:param name="AMOUNT" value="@amount"/>
    <sch:param name="AMOUNTX" value="@extended"/>
  </sch:pattern>
</sch:schema>
