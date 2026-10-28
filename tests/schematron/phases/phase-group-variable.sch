<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="p">
    <sch:let name="limit" value="2"/>
    <sch:active pattern="g"/>
  </sch:phase>
  <sch:group id="g">
    <sch:rule context="item">
      <sch:assert test="@n &lt;= $limit">Within Item <sch:value-of select="@n"/>.</sch:assert>
    </sch:rule>
    <sch:rule context="item">
      <sch:report test="@n = $limit">At the limit.</sch:report>
    </sch:rule>
  </sch:group>
</sch:schema>
